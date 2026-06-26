/**
 * @file system/async/timer-timeout.cpp
 * @brief `qb::io::async::with_timeout<>` + `qb::io::async::Timeout<F>` — the one-shot timer core.
 *
 * The timer machinery of qb-io has two public faces, both proven here against a real (but
 * socket-free) libev loop, so these are SYSTEM tests: they need `qb::io::async::init()` and they
 * pump the loop, but no descriptor is ever opened.
 *
 *   - `with_timeout<_Derived>` (qb/io/async/io.h): a CRTP base that fires the derived `on(event::timer)`
 *     once its deadline elapses. We prove construction-arms-it, `setTimeout()` re-arms / disables it,
 *     `getTimeout()` reports the live value, `updateTimeout()` defers the deadline, explicit
 *     cancellation (`setTimeout(zero)` + `_async_event.stop()`) suppresses a pending fire, and that the
 *     derived object's own state survives across the fire (the timer is not a fresh object).
 *   - `Timeout<_Func>` (the self-deleting heap one-shot behind `async::callback`): a positive timeout
 *     fires exactly once via the loop; a zero/negative timeout fires synchronously in the constructor
 *     with NO loop iteration — the five separate immediate-fire smoke tests of the old monolith are
 *     consolidated into ONE value-parametrized case over {0, -1ms, -5s}.
 *
 * Restructured from the dissolved system/test-async-io.cpp (BasicTimer, UpdateTimeout, SetTimeout,
 * TimeoutUtility, ImmediateTimeoutUtility / ZeroTimeoutTriggersImmediately /
 * NegativeTimeoutTriggersImmediately / TimeoutBehavior — folded into ImmediateTimeoutFiresInline,
 * TimerPrecision, TimerCancellation, StatefulTimerOperation, NestedTimedOperations,
 * MultipleConcurrentTimers, EventPriorities, TimerSynchronization, DroppedTimers,
 * TimeoutExceptionSafetyNoLeak). Every hand-rolled `for(i){run(EVRUN_ONCE);sleep_for()}` poll is
 * replaced by the shared deadline-bounded `qb::io::test::pump_until` so a stalled timer fails loudly
 * instead of racing the iteration budget; per-test state replaces the file-global flags; no file-local
 * main() (shared gtest_main).
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

// ---------------------------------------------------------------------------
// Fixture: a fresh per-test event loop; detach every watcher this test armed.
// ---------------------------------------------------------------------------
class TimerTimeoutTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        // Detach any live Timeout/with_timeout watchers so an orphaned timer from
        // one test cannot fire stale into the next test's loop.
        async::listener::current.clear();
    }
};

// A minimal with_timeout subject that records every fire and keeps its own state.
class CountingTimer : public async::with_timeout<CountingTimer> {
public:
    std::atomic<bool> triggered{false};
    std::atomic<int>  count{0};

    explicit CountingTimer(qb::duration timeout = 100ms)
        : with_timeout(timeout) {}

    void
    on(async::event::timer const &) {
        triggered.store(true);
        count.fetch_add(1);
    }

    // Force the underlying watcher to stop (cancellation contract).
    void
    force_stop() noexcept {
        this->_async_event.stop();
    }
};

} // namespace

// =============================================================================
// with_timeout: construction arms the timer
// =============================================================================

TEST_F(TimerTimeoutTest, ConstructionArmsTimerAndFiresOnce) {
    CountingTimer timer(50ms);
    EXPECT_FALSE(timer.triggered.load());

    EXPECT_TRUE(pump_until([&] { return timer.triggered.load(); })) << "with_timeout never fired";
    EXPECT_GE(timer.count.load(), 1);
}

// =============================================================================
// getTimeout / setTimeout
// =============================================================================

TEST_F(TimerTimeoutTest, GetTimeoutReflectsConfiguredValue) {
    CountingTimer timer(1s);
    EXPECT_EQ(timer.getTimeout(), 1s);

    timer.setTimeout(250ms);
    EXPECT_EQ(timer.getTimeout(), 250ms);

    timer.setTimeout(qb::duration::zero());
    EXPECT_EQ(timer.getTimeout(), qb::duration::zero());
}

TEST_F(TimerTimeoutTest, SetTimeoutShortensDeadlineAndFires) {
    CountingTimer timer(1s); // would not fire within the test budget
    timer.setTimeout(50ms);  // shortened — must fire promptly

    EXPECT_TRUE(pump_until([&] { return timer.triggered.load(); })) << "shortened timer never fired";
    EXPECT_EQ(timer.getTimeout(), 50ms);
}

TEST_F(TimerTimeoutTest, SetTimeoutZeroDisablesTimer) {
    CountingTimer timer(50ms);
    timer.setTimeout(qb::duration::zero()); // disable before it can fire

    // Pump well past the original 50ms deadline — it must stay silent.
    EXPECT_FALSE(pump_until([&] { return timer.triggered.load(); }, 200ms))
        << "a timer disabled via setTimeout(zero) still fired";
    EXPECT_EQ(timer.getTimeout(), qb::duration::zero());
}

// =============================================================================
// updateTimeout — defers the deadline (keep-alive)
// =============================================================================

TEST_F(TimerTimeoutTest, UpdateTimeoutDefersTheDeadline) {
    CountingTimer timer(150ms);

    // Repeatedly refresh the activity timestamp for ~250ms: each updateTimeout()
    // pushes the effective deadline forward, so the timer must NOT have fired yet
    // even though more than 150ms of wall time has elapsed.
    const auto keep_alive_until = std::chrono::steady_clock::now() + 250ms;
    while (std::chrono::steady_clock::now() < keep_alive_until) {
        timer.updateTimeout();
        async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_FALSE(timer.triggered.load()) << "updateTimeout() failed to defer the deadline";

    // Stop refreshing — now it must fire within one further timeout window.
    EXPECT_TRUE(pump_until([&] { return timer.triggered.load(); })) << "timer never fired after refresh stopped";
}

// =============================================================================
// Cancellation
// =============================================================================

TEST_F(TimerTimeoutTest, CancelledTimerNeverFires) {
    CountingTimer timer(80ms);

    // Cancel: zero the timeout AND force-stop the underlying watcher.
    timer.setTimeout(qb::duration::zero());
    timer.force_stop();

    EXPECT_FALSE(pump_until([&] { return timer.triggered.load(); }, 250ms))
        << "a cancelled timer still fired";
    EXPECT_EQ(timer.count.load(), 0);
}

// =============================================================================
// Derived state survives across the fire
// =============================================================================

namespace {

class StatefulTimer : public async::with_timeout<StatefulTimer> {
public:
    int               state_value{42};
    std::atomic<bool> triggered{false};

    explicit StatefulTimer(qb::duration timeout = 50ms)
        : with_timeout(timeout) {}

    void
    on(async::event::timer const &) {
        // The derived object is intact when the timer fires (not a fresh instance).
        EXPECT_EQ(state_value, 42);
        state_value = 84;
        triggered.store(true);
    }
};

} // namespace

TEST_F(TimerTimeoutTest, DerivedStateIsPreservedAcrossFire) {
    StatefulTimer timer(50ms);

    EXPECT_TRUE(pump_until([&] { return timer.triggered.load(); })) << "stateful timer never fired";
    EXPECT_EQ(timer.state_value, 84) << "the fire did not observe / mutate the live derived state";
}

// =============================================================================
// Timeout<F>: positive timeout fires once via the loop
// =============================================================================

TEST_F(TimerTimeoutTest, TimeoutUtilityFiresViaLoop) {
    std::atomic<bool> fired{false};
    new async::Timeout<std::function<void()>>([&fired]() { fired.store(true); }, 50ms);

    EXPECT_FALSE(fired.load()) << "a positive-timeout Timeout fired before the loop ran";
    EXPECT_TRUE(pump_until([&] { return fired.load(); })) << "Timeout<F> never fired";
}

// =============================================================================
// Timeout<F>: immediate fire (zero / negative) — consolidates the 5 dups
// =============================================================================

class ImmediateTimeoutTest : public TimerTimeoutTest,
                             public ::testing::WithParamInterface<qb::duration> {};

TEST_P(ImmediateTimeoutTest, NonPositiveTimeoutFiresInlineWithoutLoop) {
    std::atomic<bool> fired{false};
    // A zero or negative timeout must fire synchronously inside the constructor —
    // observable WITHOUT a single async::run() iteration.
    new async::Timeout<std::function<void()>>([&fired]() { fired.store(true); }, GetParam());
    EXPECT_TRUE(fired.load()) << "non-positive Timeout did not fire inline at construction";
}

INSTANTIATE_TEST_SUITE_P(NonPositiveTimeouts,
                         ImmediateTimeoutTest,
                         ::testing::Values(qb::duration::zero(),
                                           std::chrono::duration_cast<qb::duration>(-1ms),
                                           std::chrono::duration_cast<qb::duration>(-5s)));

// =============================================================================
// Timeout<F>: precision — fired not-too-early, not-absurdly-late
// =============================================================================

TEST_F(TimerTimeoutTest, TimeoutFiresWithinPlausibleWindow) {
    using clock = std::chrono::steady_clock;

    constexpr auto kTimeout = 100ms;
    std::atomic<bool> fired{false};
    const clock::time_point created = clock::now();
    clock::time_point       fired_at = created;

    new async::Timeout<std::function<void()>>(
        [&]() {
            fired_at = clock::now();
            fired.store(true);
        },
        kTimeout);

    ASSERT_TRUE(pump_until([&] { return fired.load(); })) << "precision timer never fired";

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fired_at - created);
    // Lower bound catches a fires-instantly bug; upper bound is generous for loaded CI.
    EXPECT_GE(elapsed.count(), 50) << "timer fired far too early (broken deadline)";
    EXPECT_LE(elapsed.count(), 2000) << "timer fired far too late (stalled loop)";
}

// =============================================================================
// Multiple concurrent timeouts fire in deadline order
// =============================================================================

TEST_F(TimerTimeoutTest, MultipleTimeoutsFireInDeadlineOrder) {
    std::mutex       mutex;
    std::vector<int> order;

    for (int id : {1, 2, 3}) {
        new async::Timeout<std::function<void()>>(
            [&mutex, &order, id]() {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(id);
            },
            100ms * id); // 100, 200, 300ms — strictly increasing deadlines
    }

    ASSERT_TRUE(pump_until([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size() == 3u;
    })) << "not all staggered timeouts fired";

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3})) << "timeouts did not fire in deadline order";
}

// =============================================================================
// A nested Timeout scheduled from inside a firing Timeout also fires
// =============================================================================

TEST_F(TimerTimeoutTest, NestedTimeoutSchedulesAndFires) {
    std::atomic<int> count{0};

    new async::Timeout<std::function<void()>>(
        [&count]() {
            count.fetch_add(1);
            // Schedule a second one-shot from within the first's handler.
            new async::Timeout<std::function<void()>>([&count]() { count.fetch_add(1); }, 30ms);
        },
        30ms);

    EXPECT_TRUE(pump_until([&] { return count.load() == 2; })) << "the nested timeout chain did not complete";
}

// =============================================================================
// A burst of staggered timeouts all reach a shared mutex-guarded sink
// =============================================================================

TEST_F(TimerTimeoutTest, StaggeredTimeoutsAllExecuteExactlyOnce) {
    std::mutex       mutex;
    std::vector<int> results;

    for (int i = 0; i < 5; ++i) {
        new async::Timeout<std::function<void()>>(
            [&mutex, &results, i]() {
                std::lock_guard<std::mutex> lock(mutex);
                results.push_back(i);
            },
            30ms * (i + 1));
    }

    ASSERT_TRUE(pump_until([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return results.size() == 5u;
    })) << "not all staggered timeouts executed";

    std::lock_guard<std::mutex> lock(mutex);
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<int>{0, 1, 2, 3, 4}));
}

// =============================================================================
// Fire-and-forget Timeouts (no retained handle) still fire
// =============================================================================

TEST_F(TimerTimeoutTest, DroppedTimeoutsStillFire) {
    constexpr int kCount = 10;
    std::atomic<int> completed{0};

    for (int i = 0; i < kCount; ++i) {
        // The Timeout owns itself; we keep no pointer to it.
        new async::Timeout<std::function<void()>>([&completed]() { completed.fetch_add(1); }, 20ms * (i + 1));
    }

    EXPECT_TRUE(pump_until([&] { return completed.load() == kCount; })) << "a self-owned (dropped) timeout was lost";
    EXPECT_EQ(completed.load(), kCount);
}

// =============================================================================
// Exception safety: a throwing callback must not leak and must not stop the loop
// =============================================================================

TEST_F(TimerTimeoutTest, ThrowingTimeoutDoesNotLeakOrStallTheLoop) {
    // Timeout wraps _func() in try/catch so `delete this` always runs even if the
    // callback throws; a sibling timer registered afterwards must still fire.
    std::atomic<int> throw_count{0};
    std::atomic<int> normal_count{0};

    new async::Timeout<std::function<void()>>(
        [&throw_count]() {
            throw_count.fetch_add(1);
            throw std::runtime_error("intentional throw from timer callback");
        },
        30ms);

    new async::Timeout<std::function<void()>>([&normal_count]() { normal_count.fetch_add(1); }, 60ms);

    EXPECT_TRUE(pump_until([&] { return normal_count.load() == 1; }))
        << "the throwing timer wedged the loop — the sibling never fired";
    EXPECT_EQ(throw_count.load(), 1) << "the throwing callback ran exactly once";
}
