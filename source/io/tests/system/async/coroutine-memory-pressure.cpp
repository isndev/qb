/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/coroutine-memory-pressure.cpp
 * @brief Heap allocation pressure inside coroutine frames — large payloads, many small allocations.
 *
 * Coroutines routinely hold heap data on their frame and across suspensions. These tests confirm
 * that large and numerous allocations inside coroutine bodies are written, read back correctly, and
 * freed when the coroutine completes — under the scheduler and across `sleep` suspensions, hence
 * SYSTEM tier. Waits use the shared bounded pump `qb::io::test::pump_until` (loud bounded timeout).
 *
 * What it proves:
 *   - a 1 MB `unique_ptr<char[]>` is allocated, every page touched, and the coroutine completes;
 *   - 10 coroutines each holding a 1 MB payload all complete;
 *   - 100 small `unique_ptr<int>` allocations each verify their value survived a suspension and the
 *     batch completes (`count` exactly).
 *
 * Consolidated per the audit: the alloc-pressure tests from coroutine/test-coroutine-edge-cases.cpp
 * (`LargeDataInCoroutine`) and coroutine/test-coroutine-safety.cpp (`LargeAllocation`,
 * `ManySmallAllocations`) — the same "alloc / touch / complete" shape — are merged into this single
 * file. The non-alloc edge-case/safety tests live in scheduler-stress.cpp and
 * coroutine-capture-safety.cpp.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class CoroutineMemoryPressure : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

constexpr std::size_t kPayloadBytes = 1024 * 1024; // 1 MB

} // namespace

TEST_F(CoroutineMemoryPressure, LargeAllocationTouchedAndCompletes) {
    std::atomic<bool> success{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto data = std::make_unique<char[]>(kPayloadBytes);
        for (std::size_t i = 0; i < kPayloadBytes; i += 4096)
            data[i] = static_cast<char>(i % 256);
        // Read back a couple of touched pages to prove the writes landed.
        success = (data[0] == 0 && data[4096] == static_cast<char>(4096 % 256));
        done    = true;
        co_return; // make this a coroutine: the lambda's task<void> body has no other co_*
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "large-allocation coroutine never ran";
    EXPECT_TRUE(success.load());
}

TEST_F(CoroutineMemoryPressure, ManyLargePayloadCoroutinesAllComplete) {
    constexpr int    coro_count = 10;
    std::atomic<int> completed{0};

    for (int i = 0; i < coro_count; ++i) {
        coro_scheduler().spawn([&completed]() -> task<void> {
            auto data = std::make_unique<std::vector<char>>(kPayloadBytes);
            for (std::size_t j = 0; j < data->size(); j += 4096)
                (*data)[j] = static_cast<char>(j % 256);
            co_await sleep(10ms); // hold the payload across a suspension
            completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == coro_count; })) << "large-payload batch stalled";
    EXPECT_EQ(completed.load(), coro_count);
}

TEST_F(CoroutineMemoryPressure, ManySmallAllocationsEachVerifyValue) {
    constexpr int    count = 100;
    std::atomic<int> completed{0};

    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn([&completed, i]() -> task<void> {
            auto data = std::make_unique<int>(i);
            co_await sleep(1ms); // the allocation must survive the suspension
            if (*data == i)
                completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == count; })) << "small-allocation batch stalled";
    EXPECT_EQ(completed.load(), count);
}
