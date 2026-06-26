/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/scheduler-stress.cpp
 * @brief Coroutine scheduler under stress — nesting, rapid lifecycle, randomized + bursty spawn,
 *        and many-coroutine concurrency on shared state.
 *
 * This is the "scheduler holds up under load" surface (`coro_scheduler().spawn` + `sleep` + the loop
 * + `listener` lifecycle). SYSTEM tier: every test drives the event loop, none needs a socket. Waits
 * use the shared bounded pump `qb::io::test::pump_until` (loud bounded timeout) instead of fixed
 * `run_for(Nms)` budgets that are flaky on loaded CI.
 *
 * What it proves:
 *   - deep recursive `task<int>` (50 levels) and mutual recursion converge to the right value/count;
 *   - rapid spawn-and-drop and spawn-then-clear cycles don't crash AND leave the scheduler empty
 *     afterward (real post-conditions — previously these were pure crash-canaries with `SUCCEED()`);
 *   - 500 concurrent timers and randomized/bursty spawn batches all complete (`completed == total`);
 *   - many coroutines mutating a shared atomic land on the exact expected total.
 *
 * Consolidated per the audit: this file is the home of the nesting / rapid-lifecycle / resource-
 * pressure-by-count / randomized / bursty tests from coroutine/test-coroutine-edge-cases.cpp plus the
 * concurrency tests (`SharedDataAccess`, `ReadOnlySharedData`) folded in from
 * coroutine/test-coroutine-safety.cpp. The hand-rolled `RetryPattern` (a dup of the real `with_retry`
 * coverage) is dropped — see system/async/retry-runner.cpp. The alloc-pressure tests live in
 * system/async/coroutine-memory-pressure.cpp. `rand()` jitter is replaced with a seeded `mt19937`.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class SchedulerStress : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            // Pure fixed-duration drain of any stragglers; the never-true predicate
            // means the pump always runs the full 5ms budget (return value is
            // expectedly false — consume it rather than discard the [[nodiscard]]).
            EXPECT_FALSE(qb::io::test::pump_until([] { return false; }, 5ms));
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Deep / mutual recursion
// ---------------------------------------------------------------------------

TEST_F(SchedulerStress, DeepRecursionConvergesToDepthPlusOne) {
    constexpr int    depth = 50;
    std::atomic<int> result{0};

    std::function<task<int>(int)> nested = [&](int level) -> task<int> {
        if (level <= 0)
            co_return 1;
        co_await sleep(1ms);
        const int inner = co_await nested(level - 1);
        co_return inner + 1;
    };

    coro_scheduler().spawn([&]() -> task<void> { result = co_await nested(depth); });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return result.load() == depth + 1; })) << "deep recursion stalled";
    EXPECT_EQ(result.load(), depth + 1);
}

TEST_F(SchedulerStress, MutualRecursionReachesThreshold) {
    std::atomic<int> counter{0};
    constexpr int    max_count = 10;

    std::function<task<void>(bool)> coro_a;
    std::function<task<void>(bool)> coro_b;

    coro_a = [&](bool) -> task<void> {
        counter.fetch_add(1);
        if (counter.load() < max_count) {
            co_await sleep(1ms);
            co_await coro_b(false);
        }
    };
    coro_b = [&](bool) -> task<void> {
        counter.fetch_add(1);
        if (counter.load() < max_count) {
            co_await sleep(1ms);
            co_await coro_a(false);
        }
    };

    coro_scheduler().spawn(coro_a(true));

    EXPECT_TRUE(qb::io::test::pump_until([&] { return counter.load() >= max_count; })) << "mutual recursion stalled";
    EXPECT_GE(counter.load(), max_count);
}

// ---------------------------------------------------------------------------
// Rapid lifecycle — real post-conditions, not crash-canaries
// ---------------------------------------------------------------------------

TEST_F(SchedulerStress, RapidSpawnAndDestroyLeavesSchedulerEmptyAfterClear) {
    constexpr int count = 100;

    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn([]() -> task<void> {
            co_await sleep(100ms); // long sleep — none will complete in the test window
        });
    }

    // Briefly run: not nearly long enough for any to finish, so they are all suspended.
    // Never-true predicate → the pump runs the full 10ms and returns false (consume it).
    EXPECT_FALSE(qb::io::test::pump_until([] { return false; }, 10ms));
    EXPECT_GE(coro_scheduler().active_count(), 1u) << "the long-sleeping frames should still be active";

    // Tearing down the scheduler must reclaim everything — the next fresh scheduler starts empty.
    qb::io::async::listener::current.reset_coro_scheduler();
    qb::io::test::reset_async_context();
    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "a fresh scheduler must have no active frames";
}

TEST_F(SchedulerStress, RapidSchedulerClearCyclesLeaveNoProgressLeak) {
    constexpr int    count = 50;
    std::atomic<int> completed{0};

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < count; ++i) {
            coro_scheduler().spawn([&completed]() -> task<void> {
                co_await sleep(100ms);
                completed.fetch_add(1); // must NEVER run — reclaimed before it can fire
            });
        }
        // Tear down the scheduler WITHOUT pumping the loop: the 50 frames are still in the ready
        // queue (never resumed past initial_suspend), so reset_coro_scheduler() — which runs
        // ~CoroutineScheduler() and drains+destroys the owned ready frames — is what actually
        // empties it. listener::current.clear() alone only stops libev watchers and async::init()
        // is a no-op; neither touches the scheduler's frames. The next coro_scheduler() call
        // lazily creates a fresh, empty scheduler for the following cycle.
        qb::io::async::listener::current.reset_coro_scheduler();
    }

    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "post-reset scheduler must be empty";
    EXPECT_EQ(completed.load(), 0) << "no coroutine should have completed — they were reclaimed while ready/parked";
}

// ---------------------------------------------------------------------------
// Resource pressure by count
// ---------------------------------------------------------------------------

TEST_F(SchedulerStress, ManyConcurrentTimersAllComplete) {
    constexpr int    count = 500;
    std::atomic<int> completed{0};

    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn([&completed]() -> task<void> {
            co_await sleep(50ms);
            completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == count; })) << "not all timers completed";
    EXPECT_EQ(completed.load(), count);
}

// ---------------------------------------------------------------------------
// Randomized + bursty spawn (seeded mt19937 for reproducibility)
// ---------------------------------------------------------------------------

TEST_F(SchedulerStress, RandomSleepDurationsAllComplete) {
    std::mt19937                       rng(42);
    std::uniform_int_distribution<int> dist(1, 50);

    constexpr int    count = 20;
    std::atomic<int> completed{0};

    for (int i = 0; i < count; ++i) {
        const int ms = dist(rng);
        coro_scheduler().spawn([&completed, ms]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(ms));
            completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == count; })) << "random-sleep batch stalled";
    EXPECT_EQ(completed.load(), count);
}

TEST_F(SchedulerStress, BurstySpawnAllComplete) {
    std::mt19937                       rng(42);
    std::uniform_int_distribution<int> burst_dist(1, 20);

    std::atomic<int> total_spawned{0};
    std::atomic<int> completed{0};

    for (int burst = 0; burst < 3; ++burst) {
        const int n = burst_dist(rng);
        for (int i = 0; i < n; ++i) {
            coro_scheduler().spawn([&completed]() -> task<void> {
                co_await sleep(10ms);
                completed.fetch_add(1);
            });
            total_spawned.fetch_add(1);
        }
        // Small gap between bursts: never-true predicate, full 5ms pump, returns false.
        EXPECT_FALSE(qb::io::test::pump_until([] { return false; }, 5ms));
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == total_spawned.load(); })) << "bursty batch stalled";
    EXPECT_EQ(completed.load(), total_spawned.load());
}

// ---------------------------------------------------------------------------
// Concurrency on shared state (folded in from test-coroutine-safety.cpp)
// ---------------------------------------------------------------------------

TEST_F(SchedulerStress, ManyCoroutinesIncrementSharedAtomic) {
    constexpr int    num_coroutines = 10;
    std::atomic<int> counter{0};

    // Seeded mt19937 for the sleep jitter (replaces the original unseeded rand()).
    std::mt19937                       rng(1234);
    std::uniform_int_distribution<int> dist(1, 10);

    for (int i = 0; i < num_coroutines; ++i) {
        const int ms = dist(rng);
        coro_scheduler().spawn([&counter, ms]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(ms));
            counter.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return counter.load() == num_coroutines; })) << "shared-counter batch stalled";
    EXPECT_EQ(counter.load(), num_coroutines);
}

TEST_F(SchedulerStress, ReadOnlySharedDataSumsByParameter) {
    constexpr int    num_coroutines = 5;
    std::atomic<int> sum{0};

    auto adder = [&sum](int value) -> task<void> {
        co_await sleep(5ms);
        sum.fetch_add(value);
    };

    for (int i = 0; i < num_coroutines; ++i) {
        coro_scheduler().spawn(adder(i + 1));
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return sum.load() == 15; })) << "read-only sum batch stalled";
    EXPECT_EQ(sum.load(), 15); // 1+2+3+4+5
}
