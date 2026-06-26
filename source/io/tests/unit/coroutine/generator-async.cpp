/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/generator-async.cpp
 * @brief `qb::io::async::async_generator<T>` + the `ag_*` combinators — suspending lazy sequences.
 *
 * The asynchronous half of the former test-coroutine-generator.cpp split (the pure-logic
 * synchronous `generator<T>` lives in unit/coroutine/generator-sync.cpp). An
 * `async_generator<T>` may `co_await` between yields, so it is driven through the qb-io
 * scheduler via symmetric transfer (the generator's `final_suspend`/`yield_value` tail-call
 * the awaiting consumer). These tests therefore DO spin the event loop — but with no socket,
 * timer-only — so they stay unit tier. Each drives a shared `range_async(n)` source (it
 * `co_await sleep(1ms)` then `co_yield i`) through one `ag_*` combinator and asserts the exact
 * collected vector/scalar.
 *
 * Covered combinators: `ag_for_each` (sync + suspending callback), `ag_collect`, `ag_map`,
 * `ag_filter`, `ag_reduce`, `ag_take`, `ag_skip`, the empty-generator edge, and
 * `async_generator` move semantics. Re-homes the `ag_collect` symmetric-transfer regression.
 *
 * De-flake: every test gates on a real completion flag through the shared
 * `qb::io::test::pump_until` (loud bounded timeout) instead of a blind `run_for(Nms)` window
 * racing the 1ms sleeps. Hardening over the original: the move-semantics test compares the
 * FULL collected vector after the move (not just `.size()`), and all of the shipped
 * `QB_DEBUG_AGEN`/`QB_DEBUG_SCOPE`/`TLOG` stderr instrumentation is dropped.
 */

#include <atomic>
#include <functional>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class GeneratorAsync : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

// Shared async source: sleeps 1ms then yields i, for i in [0, n).
async_generator<int>
range_async(int n) {
    for (int i = 0; i < n; ++i) {
        co_await sleep(1ms);
        co_yield i;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ag_for_each
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, AgForEachVisitsAllValues) {
    std::vector<int>  visited;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&visited, &done]() -> task<void> {
        co_await ag_for_each(range_async(5), [&visited](int v) { visited.push_back(v); });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_for_each never finished";
    EXPECT_EQ(visited, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(GeneratorAsync, AgForEachWithSuspendingCallbackVisitsAll) {
    std::vector<int>  visited;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&visited, &done]() -> task<void> {
        co_await ag_for_each(range_async(3), [&visited](int v) -> task<void> {
            co_await sleep(1ms);
            visited.push_back(v * 10);
        });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_for_each (async callback) never finished";
    EXPECT_EQ(visited, (std::vector<int>{0, 10, 20}));
}

// ---------------------------------------------------------------------------
// ag_collect / ag_map / ag_filter / ag_reduce
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, AgCollectGathersAll) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(range_async(5));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_collect never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(GeneratorAsync, AgMapTransformsAll) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_map(range_async(4), [](int v) { return v * 2; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_map never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 2, 4, 6}));
}

TEST_F(GeneratorAsync, AgFilterKeepsMatching) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_filter(range_async(6), [](int v) { return v % 2 == 0; });
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_filter never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 2, 4}));
}

TEST_F(GeneratorAsync, AgReduceSumsAll) {
    int               sum = -1;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&sum, &done]() -> task<void> {
        sum = co_await ag_reduce(range_async(5), 0, std::plus<int>{});
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_reduce never finished";
    EXPECT_EQ(sum, 0 + 1 + 2 + 3 + 4);
}

// ---------------------------------------------------------------------------
// ag_take / ag_skip
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, AgTakeLimitsOutput) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(ag_take(range_async(10), 3));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_take never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2}));
}

TEST_F(GeneratorAsync, AgSkipSkipsFirstN) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(ag_skip(range_async(6), 3));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_skip never finished";
    EXPECT_EQ(result, (std::vector<int>{3, 4, 5}));
}

// ---------------------------------------------------------------------------
// Edge: empty async generator
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, EmptyAsyncGeneratorYieldsNothing) {
    std::vector<int>  result{99}; // seeded so a non-running coroutine fails
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&result, &done]() -> task<void> {
        result = co_await ag_collect(range_async(0));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "empty ag_collect never finished";
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// Move semantics — full-element verification (not just size)
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, MoveTransfersTheEntireSequence) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto gen1 = range_async(3);
        auto gen2 = std::move(gen1); // moved-from gen1 owns nothing; gen2 owns the frame
        result    = co_await ag_collect(std::move(gen2));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "moved async generator never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 1, 2})) << "the moved-to async generator must yield the full original sequence";
}

// ---------------------------------------------------------------------------
// Regression: ag_collect drives the symmetric-transfer yield chain
// ---------------------------------------------------------------------------

TEST_F(GeneratorAsync, AgCollectSymmetricTransferRegression) {
    std::vector<int>  result;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto gen = []() -> async_generator<int> {
            for (int i = 0; i < 5; ++i)
                co_yield i * 10;
        };
        result = co_await ag_collect(gen());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ag_collect symmetric-transfer never finished";
    EXPECT_EQ(result, (std::vector<int>{0, 10, 20, 30, 40}));
}
