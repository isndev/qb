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
#include "../../shared/coroutine_reclaim_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reclaim_fast_winner;
using qb::io::test::run_reclaim_driver;

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

// =============================================================================
// Destroy-while-parked reclamation (see shared/coroutine_reclaim_support.h)
//
// A consumer parked in async_generator::next() stores its handle in the generator's `continuation`;
// when its frame is reclaimed (when_any loser) the next_awaiter dtor must clear it so a later
// co_yield does not symmetric-transfer into the freed frame.
// =============================================================================

TEST_F(GeneratorAsync, NextReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        auto gen = std::make_shared<async_generator<int>>([]() -> async_generator<int> {
            co_await sleep(40ms);
            co_yield 1;
            co_yield 2;
        }());
        auto park = [](std::shared_ptr<async_generator<int>> g) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await g->next();
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto r = co_await when_any(park(gen), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(60ms); // the generator wakes and yields into the (reclaimed) continuation
    });
}

// =============================================================================
// The GENERATOR's OWN frame is destroyed while QUEUED. The generator internally awaits a wait-list
// primitive (mutex); next() parks the GENERATOR frame on it. A winner unlocks (the generator frame
// is woken → queued in the scheduler) then completes in one resume, so the when_any reclaim destroys
// the parker — whose only ref to the async_generator drops → ~async_generator destroys the queued
// generator frame. Without async_generator's forget_frame_if_current scrub that handle would dangle
// in ready_queue_ → heap-UAF in run_ready(). The generator frame carries a >4 KiB local so it
// escapes the coroutine frame pool and ASan sees the free.
// =============================================================================

TEST_F(GeneratorAsync, GeneratorOwnFrameWokenThenDestroyedNoUAF) {
    run_reclaim_driver([]() -> task<void> {
        auto m = std::make_shared<async_mutex>();
        m->try_lock(); // pre-hold so the generator's internal lock() must park the generator frame
        auto park = [](std::shared_ptr<async_mutex> mm) -> task<int> {
            // gen is owned ONLY here: reclaiming this parker drops the last ref → ~async_generator.
            auto gen = std::make_shared<async_generator<int>>([](std::shared_ptr<async_mutex> m2) -> async_generator<int> {
                volatile char big[8192];
                big[0] = 7;
                co_await m2->lock(); // parks the GENERATOR frame on the mutex
                big[1] = big[0];
                co_yield (int) big[1];
            }(mm));
            auto v = co_await gen->next(); // drives the generator until it parks on the mutex
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto winner = [](std::shared_ptr<async_mutex> mm) -> task<int> {
            co_await sleep(5ms); // let the generator park on the mutex first
            mm->unlock();        // wakes the generator frame -> queued in ready_queue_
            co_return 99;        // -> when_any reclaim destroys the parker -> ~async_generator (queued frame)
        };
        auto r = co_await when_any(park(m), winner(m));
        EXPECT_EQ(r.index, 1u) << "the unlocking winner must win the race";
        co_await sleep(20ms);
    });
}

// =============================================================================
// Over-pull / moved-from async_generator::next() must return nullopt, never UB. await_ready returns
// false unconditionally would symmetric-transfer into a done() handle (resuming a coroutine at its
// final-suspend point = UB) or a null handle (moved-from) on the extra pull. Guards mirror the sync
// generator<T>::next(). This is a deterministic, sanitizer-checked test (no reclaim machinery).
// =============================================================================

TEST_F(GeneratorAsync, NextOverPullAndMovedFromReturnNullopt) {
    std::atomic<bool> done{false};
    std::atomic<int>  v1{-1};
    std::atomic<bool> b_empty{false}, c_empty{false}, d_empty{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto gen = []() -> async_generator<int> { co_yield 7; }();
        auto a   = co_await gen.next(); // 7
        auto b   = co_await gen.next(); // clean end -> nullopt
        auto c   = co_await gen.next(); // OVER-PULL on a done generator -> nullopt (was UB)
        async_generator<int> moved = std::move(gen);
        auto d = co_await gen.next(); // MOVED-FROM (null handle) -> nullopt (was UB)
        v1.store(a.value_or(-1));
        b_empty.store(!b.has_value());
        c_empty.store(!c.has_value());
        d_empty.store(!d.has_value());
        done.store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "over-pull generator coroutine never ran";
    EXPECT_EQ(v1.load(), 7);
    EXPECT_TRUE(b_empty.load()) << "clean end must be nullopt";
    EXPECT_TRUE(c_empty.load()) << "over-pull on a done generator must be nullopt, not UB";
    EXPECT_TRUE(d_empty.load()) << "next() on a moved-from generator must be nullopt, not UB";
}
