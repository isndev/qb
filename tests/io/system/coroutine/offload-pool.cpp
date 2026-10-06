/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/offload-pool.cpp
 * @brief The offload pool's lifecycle (qb/io/async/coroutine/offload.h, Huly QB-69): no thread before the first
 *        `offload`, and `set_offload_threads` honoured before it only.
 *
 * ALONE in its binary, on purpose. The pool is process-wide and starts once: this case needs a process in which nothing
 * has offloaded yet, and every ctest run of a GoogleTest binary is shuffled (`--gtest_shuffle`, QB_TESTS_SHUFFLE), so
 * "the first case of the file" is not the first case of the run. It sat first in offload.cpp and failed whenever a
 * seed put another case ahead of it -- three of the five presets of the Windows gate on 2026-10-06.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

// A named namespace: the fixture's test body is handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace offload_pool_test {

class OffloadPool : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }
};

} // namespace offload_pool_test

using offload_pool_test::OffloadPool;

TEST_F(OffloadPool, NoThreadBeforeTheFirstOffloadAndTheSizeIsTakenOnlyBeforeIt) {
    // The pool starts once per process, so only the first run in a process has something to observe; ctest runs this
    // binary once, and a second run (`--gtest_repeat`) says so rather than failing on the pool the first one started.
    static bool ran_in_this_process = false;
    if (std::exchange(ran_in_this_process, true))
        GTEST_SKIP() << "the pool started in an earlier run of this process; it starts once";
    ASSERT_EQ(current_offload_stats().threads, 0u) << "a pool thread exists before any offload";
    EXPECT_FALSE(set_offload_threads(0)) << "a pool of zero threads was accepted";
    ASSERT_TRUE(set_offload_threads(3));
    ASSERT_EQ(current_offload_stats().threads, 0u) << "sizing the pool started it";

    // Three calls that each wait until all three run at once: they finish only if the pool really
    // has three threads (with two, the third never starts while the first two wait, and each call
    // gives up after its limit and reports it).
    std::mutex              m;
    std::condition_variable cv;
    int                     arrived    = 0;
    auto                    rendezvous = [&m, &cv, &arrived] {
        std::unique_lock lk(m);
        ++arrived;
        cv.notify_all();
        return cv.wait_for(lk, 5s, [&arrived] { return arrived == 3; });
    };
    std::atomic<int> together{0};
    std::atomic<int> done{0};
    for (int i = 0; i < 3; ++i)
        coro_scheduler().spawn([&rendezvous, &together, &done]() -> task<void> {
            if (co_await offload(rendezvous))
                together.fetch_add(1);
            done.fetch_add(1);
        });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 3; }, 10s)) << "the three offloads never completed";
    EXPECT_EQ(together.load(), 3) << "the three calls never ran at once: the pool does not have the three threads it was given";
    EXPECT_EQ(current_offload_stats().threads, 3u);
    EXPECT_FALSE(set_offload_threads(4)) << "the pool was resized after it started";
}
