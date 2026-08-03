/**
 * @file system/async/timer-stress.cpp
 * @brief Bulk / concurrent / multi-threaded timer-dispatch stress for the qb-io async loop.
 *
 * These are the correctness-under-load cases for the timer machinery (`with_timeout<>` and the
 * self-deleting `Timeout<F>` behind `async::callback`). They drive a real (socket-free) libev loop, so
 * they are SYSTEM tests: many timers in flight at once, bulk create/destroy churn, and several
 * independent per-thread loops. The framing of the old monolith called these "performance" tests; they
 * are not benchmarks — they assert exact completion counts and leave the throughput numbers to a
 * dedicated `benchmark/` target. The point proven here is that the loop dispatches EVERY armed timer
 * and reclaims EVERY destroyed one with no loss and no leak.
 *
 * Contracts:
 *   - N concurrent `with_timeout` handlers all fire (exact count, not a smoke `GE`);
 *   - bulk `Timeout`s churned through create → fire → bulk-destroy leave the watcher registry empty
 *     (`listener::current.size()` returns to baseline — the real leak guard that replaces the old
 *     `EXPECT_TRUE(true)`);
 *   - destroying half the handlers mid-flight is safe (no use-after-free, no crash);
 *   - each of several worker threads runs its OWN loop and fires all of its own timers;
 *   - a high-volume burst on one loop completes in full.
 *
 * Restructured from the dissolved system/test-async-io.cpp (ManyConcurrentTimers, TimerMemoryUsage,
 * ResourceCleanup, MultiThreadedAsyncOperations, AsyncInitCleanupThreads, IntensiveAsyncOperations,
 * MultipleConcurrentTimers). Every hand-rolled poll loop is replaced by the shared
 * `qb::io::test::pump_until`; the vacuous `EXPECT_TRUE(true)` cleanup assertion becomes a watcher-count
 * leak guard; `EXPECT_GE`-style smoke counts become exact `EXPECT_EQ`. No file-local main().
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

class TimerStressTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

// A handler that flips a flag on fire — used to count completions deterministically.
class FlagTimer : public async::with_timeout<FlagTimer> {
public:
    std::atomic<bool> triggered{false};

    explicit FlagTimer(qb::duration timeout)
        : with_timeout(timeout) {}

    void
    on(async::event::timer const &) {
        triggered.store(true);
    }
};

// Count how many handlers in the vector have fired.
int
count_fired(const std::vector<std::unique_ptr<FlagTimer>> &timers) {
    int fired = 0;
    for (const auto &t : timers)
        if (t->triggered.load())
            ++fired;
    return fired;
}

} // namespace

// =============================================================================
// Many concurrent with_timeout handlers all fire (exact count)
// =============================================================================

TEST_F(TimerStressTest, ManyConcurrentTimersAllFire) {
    constexpr int kCount = 50;

    std::vector<std::unique_ptr<FlagTimer>> timers;
    timers.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
        timers.push_back(std::make_unique<FlagTimer>(10ms * (i % 10 + 1)));

    EXPECT_TRUE(pump_until([&] { return count_fired(timers) == kCount; }))
        << "only " << count_fired(timers) << "/" << kCount << " concurrent timers fired";
    EXPECT_EQ(count_fired(timers), kCount);
}

// =============================================================================
// Bulk create → fire → destroy leaves the watcher registry empty (leak guard)
// =============================================================================

TEST_F(TimerStressTest, BulkTimersFireThenDestroyLeavesNoLiveWatchers) {
    constexpr int kCount = 100;

    const auto baseline = async::listener::current.size();

    std::vector<std::unique_ptr<FlagTimer>> timers;
    timers.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
        timers.push_back(std::make_unique<FlagTimer>(10ms));

    // Every handler registered a watcher.
    EXPECT_EQ(async::listener::current.size(), baseline + kCount);

    EXPECT_TRUE(pump_until([&] { return count_fired(timers) == kCount; })) << "not all bulk timers fired before teardown";

    // Destroy them all and pump once to settle any pending teardown.
    timers.clear();
    async::run(EVRUN_NOWAIT);

    // The real cleanup contract (replaces the old EXPECT_TRUE(true)): no leaked watcher.
    EXPECT_EQ(async::listener::current.size(), baseline) << "destroyed timers left live watchers registered";
}

// =============================================================================
// Destroying half the handlers mid-flight is safe
// =============================================================================

TEST_F(TimerStressTest, DestroyingHalfTheTimersMidFlightIsSafe) {
    constexpr int kCount = 10;

    std::vector<std::unique_ptr<FlagTimer>> timers;
    timers.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
        timers.push_back(std::make_unique<FlagTimer>(100ms));

    // Run one slice to register everything, then drop the first half before they fire.
    async::run(EVRUN_NOWAIT);
    for (int i = 0; i < kCount / 2; ++i)
        timers[i].reset();

    // The surviving half must still fire cleanly; no crash / UAF from the dropped half.
    EXPECT_TRUE(pump_until([&] {
        int fired = 0;
        for (int i = kCount / 2; i < kCount; ++i)
            if (timers[i] && timers[i]->triggered.load())
                ++fired;
        return fired == kCount / 2;
    })) << "the surviving timers did not all fire after mid-flight destruction";
}

// =============================================================================
// Each worker thread runs its own loop and fires all of its own timers
// =============================================================================

TEST_F(TimerStressTest, PerThreadLoopsEachFireAllTheirTimers) {
    constexpr int kThreads         = 4;
    constexpr int kTimersPerThread = 5;

    std::atomic<int>         total_completed{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&total_completed] {
            async::init(); // independent per-thread loop
            std::atomic<int> completed{0};

            for (int i = 0; i < kTimersPerThread; ++i)
                async::callback([&completed]() { completed.fetch_add(1); }, 20ms);

            EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == kTimersPerThread; }))
                << "a worker thread did not fire all its timers";
            total_completed.fetch_add(completed.load());
        });
    }

    for (auto &th : threads)
        th.join();

    EXPECT_EQ(total_completed.load(), kThreads * kTimersPerThread);
}

// =============================================================================
// High-volume burst on a single loop completes in full
// =============================================================================

TEST_F(TimerStressTest, HighVolumeBurstCompletesInFull) {
    constexpr int    kOperations = 1000;
    std::atomic<int> completed{0};

    for (int i = 0; i < kOperations; ++i)
        async::callback([&completed]() { completed.fetch_add(1); }, 5ms);

    // 1000 short timers on one loop — generous budget, but every one must fire.
    EXPECT_TRUE(pump_until([&] { return completed.load() == kOperations; }, 10s))
        << "high-volume burst dropped timers: " << completed.load() << "/" << kOperations;
    EXPECT_EQ(completed.load(), kOperations);
}
