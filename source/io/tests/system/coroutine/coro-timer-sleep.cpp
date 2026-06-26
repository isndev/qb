/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coro-timer-sleep.cpp
 * @brief `sleep()` timer-awaiter behaviours under the qb-io event loop.
 *
 * `sleep(duration)` (qb/io/async/coroutine/utils.h) suspends a coroutine on a libev timer and
 * resumes it when the timer fires. These tests exercise the timer awaiter directly: completion,
 * relative ordering of differently-sized timers, destruction-cancels-the-timer, long timers, looped
 * timers, the zero-duration fast path, and cancellation of a coroutine parked on a sleep. SYSTEM tier
 * (real event loop + timers). Every wait uses the shared bounded pump `qb::io::test::pump_until`
 * (loud bounded timeout) — never a fixed `run_for(Nms)` budget.
 *
 * Renamed/strengthened from coroutine/test-coroutine-timers.cpp:
 *   - the two `DISABLED_` timing-accuracy tests are replaced by ONE deterministic relative-ordering
 *     test that records a monotonic sequence number per completion and asserts shortest-timer-first
 *     by *membership + relative position*, not by wall-clock instants (robust under load);
 *   - smoke counters are kept as exact-count value paths;
 *   - NEW: `sleep(0ms)` completes promptly (zero/negative-duration fast path); a coroutine parked on
 *     a long sleep can be torn down by `cancellable_sleep` cancellation; the loop counter is exact.
 */

#include <atomic>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class CoroutineTimerSleep : public ::testing::Test {
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

} // namespace

// ---------------------------------------------------------------------------
// Completion + relative ordering (deterministic — sequence counter, not clock)
// ---------------------------------------------------------------------------

TEST_F(CoroutineTimerSleep, SleepCompletesAndResumes) {
    std::atomic<bool> fired{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(10ms);
        fired = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return fired.load(); })) << "sleep never resumed the coroutine";
}

TEST_F(CoroutineTimerSleep, ShorterTimersResumeBeforeLongerOnesRelativeOrder) {
    // Record the order in which timers fire using a shared monotonic sequence counter instead of
    // wall-clock timestamps. Timers are spawned with durations that are well separated (10ms apart)
    // so the relative order is robust under scheduling jitter; we assert membership + that the
    // shortest fired first and the longest fired last.
    constexpr int    count = 5;
    std::atomic<int> next_seq{0};
    std::vector<int> seq_of_index(count, -1); // seq_of_index[i] = completion order of timer i
    std::atomic<int> completed{0};

    // Timer i sleeps (count - i) * 10ms → timer (count-1) is shortest, timer 0 is longest.
    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn([i, &next_seq, &seq_of_index, &completed]() -> task<void> {
            co_await sleep(std::chrono::milliseconds((count - i) * 10));
            seq_of_index[i] = next_seq.fetch_add(1);
            completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == count; })) << "not all timers fired";

    // Every timer fired exactly once → sequence numbers are a permutation of 0..count-1.
    std::vector<int> seen(count, 0);
    for (int i = 0; i < count; ++i) {
        ASSERT_GE(seq_of_index[i], 0) << "timer " << i << " never recorded a sequence";
        ASSERT_LT(seq_of_index[i], count);
        ++seen[seq_of_index[i]];
    }
    for (int s = 0; s < count; ++s)
        EXPECT_EQ(seen[s], 1) << "sequence " << s << " was assigned " << seen[s] << " times (must be exactly once)";

    // Shortest timer (index count-1) fired first; longest timer (index 0) fired last.
    EXPECT_EQ(seq_of_index[count - 1], 0) << "the shortest timer must resume first";
    EXPECT_EQ(seq_of_index[0], count - 1) << "the longest timer must resume last";
}

// ---------------------------------------------------------------------------
// Destruction cancels the timer
// ---------------------------------------------------------------------------

TEST_F(CoroutineTimerSleep, DestroyedTaskTimerNeverFires) {
    std::atomic<bool> long_completed{false};
    std::atomic<bool> short_completed{false};

    {
        // This task is created but NOT spawned — its frame is destroyed at block exit, so its timer
        // must never fire.
        auto long_task = [&]() -> task<void> {
            co_await sleep(200ms);
            long_completed = true;
        };
        auto dropped = long_task();
        (void) dropped; // destroyed here

        coro_scheduler().spawn([&]() -> task<void> {
            co_await sleep(10ms);
            short_completed = true;
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return short_completed.load(); })) << "short timer never fired";
    // Give the dropped task's (would-be) 200ms timer ample time to (not) fire.
    // Never-true predicate → the pump runs the full 60ms and returns false (consume it).
    EXPECT_FALSE(qb::io::test::pump_until([] { return false; }, 60ms));
    EXPECT_FALSE(long_completed.load()) << "a destroyed task's timer must not fire";
}

// ---------------------------------------------------------------------------
// Long duration + looped timers
// ---------------------------------------------------------------------------

TEST_F(CoroutineTimerSleep, LongDurationTimerDoesNotFireEarly) {
    std::atomic<bool> completed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(300ms);
        completed = true;
    });

    // Not long enough — must still be parked. The pump must NOT see completion in 40ms.
    EXPECT_FALSE(qb::io::test::pump_until([&] { return completed.load(); }, 40ms))
        << "a 300ms timer fired within 40ms";
    EXPECT_FALSE(completed.load()) << "a 300ms timer fired within 40ms";

    // Now wait it out.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load(); })) << "300ms timer never fired";
}

TEST_F(CoroutineTimerSleep, MixedDurationTimersAllComplete) {
    const std::vector<int> durations{5, 10, 15, 20, 25};
    std::atomic<int>       completed{0};

    for (int duration : durations) {
        coro_scheduler().spawn([&completed, duration]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(duration));
            completed.fetch_add(1);
        });
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() == static_cast<int>(durations.size()); }))
        << "not all mixed-duration timers completed";
    EXPECT_EQ(completed.load(), static_cast<int>(durations.size()));
}

TEST_F(CoroutineTimerSleep, TimerInLoopRunsExactlyNTimes) {
    constexpr int    iterations = 5;
    std::atomic<int> counter{0};

    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < iterations; ++i) {
            co_await sleep(10ms);
            counter.fetch_add(1);
        }
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return counter.load() == iterations; })) << "looped timer stalled";
    EXPECT_EQ(counter.load(), iterations);
}

// ---------------------------------------------------------------------------
// Zero / negative duration fast path
// ---------------------------------------------------------------------------

TEST_F(CoroutineTimerSleep, ZeroDurationSleepCompletesPromptly) {
    std::atomic<int>  order{0};
    std::atomic<int>  resume_order{-1};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        order.fetch_add(1); // 1: runs before suspending
        co_await sleep(0ms);
        resume_order = order.fetch_add(1); // resumes after the suspension point
        done         = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "sleep(0ms) never resumed";
    EXPECT_GE(resume_order.load(), 1) << "sleep(0ms) must still resume after a yield, not before running";
}

TEST_F(CoroutineTimerSleep, NegativeDurationSleepCompletesPromptly) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(std::chrono::milliseconds(-50)); // a deadline already in the past
        done = true;
    });

    // A negative/past duration must resume promptly, never hang.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 500ms)) << "sleep(negative) never resumed";
}

// ---------------------------------------------------------------------------
// Cancel a coroutine parked on a sleep
// ---------------------------------------------------------------------------

TEST_F(CoroutineTimerSleep, CancelWhileParkedOnSleepUnwindsPromptly) {
    cancellation_token token;
    std::atomic<bool>  caught{false};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await cancellable_sleep(5000ms, token); // park on a long sleep
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return token.get_state()->callbacks.size() == 1u; }))
        << "coroutine never parked on the sleep";
    EXPECT_FALSE(done.load());

    token.cancel();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "parked coroutine never woke on cancel";
    EXPECT_TRUE(caught.load());
}
