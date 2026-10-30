/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/timeout-clock.cpp
 * @brief `with_timeout`'s time left is measured on the loop's clock, not on the time of day (Huly QB-1194).
 *
 * `with_timeout` dated an activity by the loop's time of day (`ev_now()`) and measured the time left against it when
 * the timer fired: a step of the wall clock in between shortened the idle timeout, or lengthened it. Here the time of
 * day is INJECTED, never changed on the system, in both places the code could read it:
 *   - the loop's: qev's `ev_now_set`, handed a reading ahead of the loop's clock, leaves the loop in the state a step
 *     of the wall clock leaves it in at its next refresh (see `step_the_times_of_day_back`);
 *   - the system's: every `ev_time()` this file compiles -- any `with_timeout` might make, its template is
 *     instantiated here -- reads the system's wall clock plus the injected step. `with_timeout` never read it (the
 *     base read the loop's); one that did would read the injected one.
 * Each scenario runs on a thread of its own, hence on an event loop of its own (`listener::current` is per thread): a
 * loop whose time of day was moved serves no other test. A fire with no activity recorded is pinned too: it is the
 * timeout, whatever span the timer was configured for.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

// qev's header first: it declares `ev_time` (under its link name, `qb_ev_time`).
#include <qb/io/async/listener.h>

namespace timeout_clock_test {

/// The step of the system's time of day a test injects, in seconds; the system's clock is never changed.
inline double time_of_day_step = 0.;

/// How much less than the timeout an idle time measured here may read: the conversions to the loop's double seconds,
/// nothing else. A test reads `qb::mono_now()` BEFORE the activity reads the loop's clock (`updateTimeout()`);
/// `with_timeout` lets a fire through only on a fresh reading of that clock at least the activity plus the timeout;
/// the handler reads `qb::mono_now()` after it. One clock (ListenerTimerGate.TheLoopsClockIsTheClockOfMonoTime, to
/// 0.1 us), read in that order: the idle time is at least the timeout on any host, a load only delays a fire. A slack
/// of a sixteenth let an early fire through -- an activity dated by the loop's cached clock, as stale as the passes
/// the core skipped (Huly QB-190), up to 75 ms on these timeouts.
inline constexpr qb::duration kClockRounding = std::chrono::microseconds{1};

/// The system's wall clock, as the loop library reads it, plus the injected step.
[[nodiscard]] inline double
stepped_time_of_day() noexcept {
    return ev_time() + time_of_day_step;
}

} // namespace timeout_clock_test

// qb-io's headers -- `with_timeout`'s among them -- compiled with every `ev_time()` reading the stepped time of day;
// the injection is lifted right after them.
#ifdef QB_IO_ASYNC_IO_H
#error "qb/io/async/io.h was included before the time of day was injected: its with_timeout would read the real one"
#endif
#pragma push_macro("ev_time")
#undef ev_time
#define ev_time() timeout_clock_test::stepped_time_of_day()
#include <qb/io/async.h>
#pragma pop_macro("ev_time")

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace timeout_clock_test {

// An idle timer that records when it fired.
class IdleTimer : public qb::io::async::with_timeout<IdleTimer> {
public:
    std::atomic<int> fires{0};
    qb::mono_time    fired_at{};

    explicit IdleTimer(qb::duration timeout)
        : with_timeout(timeout) {}

    void
    on(qb::io::async::event::timer const &) {
        fired_at = qb::mono_now();
        fires.fetch_add(1);
    }

    // Re-arm the underlying watcher by hand, as `event::timer`'s documentation allows ("set() then start()").
    void
    rearm(qb::duration after) noexcept {
        this->_async_event.start(qb::detail::to_ev_seconds(after));
    }
};

// Both times of day `step` seconds back, the way a step of the system's wall clock moves them, without touching the
// system's: the system's at once (the injected `ev_time()`), the loop's -- `ev_now()`, which the loop re-reads from
// the wall clock at most every half second and advances on its own clock in between -- until the loop refreshes it,
// `step` + 0.5 s later, when it steps forward by `step`. The loop's own clock, the one its timers run on, is untouched.
// The loop's half: handed a reading `step` ahead -- more than the half second, which it then re-reads the wall clock
// against -- and read back at once (`ev_now_update`), its own clock returns to the true reading and its time of day
// stays behind. A step of exactly 0.5 s can miss: the sum rounds, and the loop interpolates.
void
step_the_times_of_day_back(struct ev_loop *loop, double const step) noexcept {
    ev_now_set(loop, ev_clock_now() + step);
    ev_now_update(loop);
    time_of_day_step -= step;
}

// How far the loop's time of day reads behind the system's real wall clock, as of a fresh refresh of its clock.
double
loop_lag(struct ev_loop *loop) noexcept {
    ev_now_update(loop);
    return qb::detail::to_ev_seconds(qb::wall_now().time_since_epoch()) - ev_now(loop);
}

// How far the injected system time of day reads behind the real one.
double
system_lag() noexcept {
    return qb::detail::to_ev_seconds(qb::wall_now().time_since_epoch()) - stepped_time_of_day();
}

// A scenario on a thread of its own, hence on an event loop of its own.
template <typename Scenario>
void
on_a_loop_of_its_own(Scenario &&scenario) {
    std::thread([&scenario] {
        scenario(static_cast<struct ev_loop *>(qb::io::async::listener::current.loop()));
        qb::io::async::listener::current.clear();
    }).join();
}

class TimeoutClock : public ::testing::Test {
protected:
    void
    SetUp() override {
        time_of_day_step = 0.;
    }

    void
    TearDown() override {
        time_of_day_step = 0.;
    }
};

} // namespace timeout_clock_test

using timeout_clock_test::TimeoutClock;

TEST_F(TimeoutClock, ATimeOfDayStepBackDoesNotLengthenTheIdleTimeout) {
    // An activity, then the time of day steps 30 s back. Measured on it, the timeout would be 30 s further away than
    // the activity says: the timer is not to wait for the time of day to catch up.
    using namespace timeout_clock_test;
    constexpr auto   kTimeout    = 300ms;
    constexpr double kStep       = 30.; // far beyond the pump's budget
    double           loop_lagged = 0., system_lagged = 0.;
    int              fires = 0;
    qb::duration     idle{};
    on_a_loop_of_its_own([&](struct ev_loop *loop) {
        IdleTimer timer(kTimeout);
        std::this_thread::sleep_for(100ms); // the activity comes after the arm: the fire re-arms for the rest
        const auto active = qb::mono_now(); // before the activity: `idle` is never under-read
        timer.updateTimeout();
        step_the_times_of_day_back(loop, kStep);
        loop_lagged   = loop_lag(loop);
        system_lagged = system_lag();
        (void) pump_until([&] { return timer.fires.load() > 0; }, 3s);
        fires = timer.fires.load();
        idle  = timer.fired_at - active;
    });
    ASSERT_GT(loop_lagged, kStep / 2) << "the injection did not move the loop's time of day";
    ASSERT_GT(system_lagged, kStep / 2) << "the injection did not move the system's time of day";
    ASSERT_EQ(fires, 1) << "the idle timeout waited for the time of day to catch up with the step";
    EXPECT_GE(idle, kTimeout - kClockRounding) << "the idle timeout fired early, by " << (kTimeout - idle).count() << " ns";
}

TEST_F(TimeoutClock, ATimeOfDayStepForwardDoesNotShortenTheIdleTimeout) {
    // The times of day read 0.6 s behind, an activity comes, then they step 0.6 s forward -- the system's at once, the
    // loop's at its refresh -- before the timer fires. Measured on them, the activity would look 0.6 s older than it
    // is: the timer is not to fire before a whole timeout of idleness. A host that stalls this thread past the loop's
    // refresh before the activity leaves nothing to prove: such an attempt is replayed.
    using namespace timeout_clock_test;
    constexpr auto   kTimeout   = 1200ms;
    constexpr double kStep      = 0.6; // over the least step the injection makes (see step_the_times_of_day_back)
    bool             conclusive = false;
    for (int attempt = 0; attempt < 5 && !conclusive; ++attempt) {
        double       lag_at_activity = 0.;
        double       lag_at_fire     = kStep;
        int          fires           = 0;
        qb::duration idle{};
        on_a_loop_of_its_own([&](struct ev_loop *loop) {
            step_the_times_of_day_back(loop, kStep); // the loop's refreshes, so steps forward, 1.1 s from now
            IdleTimer timer(kTimeout);
            std::this_thread::sleep_for(400ms);
            const auto active = qb::mono_now(); // before the activity: `idle` is never under-read
            timer.updateTimeout();
            lag_at_activity = loop_lag(loop);
            time_of_day_step += kStep; // the system's steps forward now
            (void) pump_until([&] { return timer.fires.load() > 0; }, 3s);
            fires       = timer.fires.load();
            idle        = timer.fired_at - active;
            lag_at_fire = loop_lag(loop);
        });
        time_of_day_step = 0.;
        if (lag_at_activity < kStep / 2)
            continue; // the loop had refreshed its time of day before the activity: nothing moved under it
        conclusive = true;
        EXPECT_LT(lag_at_fire, kStep / 2) << "the loop's time of day never stepped forward";
        ASSERT_EQ(fires, 1) << "the idle timeout never fired";
        EXPECT_GE(idle, kTimeout - kClockRounding)
            << "the idle timeout fired early, by " << (kTimeout - idle).count() << " ns: the step forward was counted as idleness";
    }
    EXPECT_TRUE(conclusive) << "no attempt recorded its activity before the loop refreshed its time of day";
}

TEST_F(TimeoutClock, ATimerReArmedByHandWithoutActivityFiresAtItsOwnDelay) {
    // With no activity recorded, a fire IS the timeout, whatever span the timer was configured for: one the derived
    // class re-arms by hand for 10 ms fires at 10 ms, though configured for 4 x 10^9 s -- more than any clock qb reads
    // has run since its epoch, the time of day's 1.8 x 10^9 s included, so a reading of either would re-arm it.
    using namespace timeout_clock_test;
    int fires = 0;
    on_a_loop_of_its_own([&](struct ev_loop *) {
        IdleTimer timer(std::chrono::seconds{4'000'000'000});
        timer.rearm(10ms);
        (void) pump_until([&] { return timer.fires.load() > 0; });
        fires = timer.fires.load();
    });
    EXPECT_EQ(fires, 1) << "a fire with no activity recorded was not the timeout";
}

TEST_F(TimeoutClock, SetTimeoutArmsFromTheLoopsClockNotFromItsLastPass) {
    // `setTimeout` arms the timer from now. The loop's cached clock is as stale as its last pass -- a core with nothing
    // to fire skips the loop (Huly QB-190) -- and an armed watcher re-armed against that cache is due early: it fires for
    // nothing and is re-armed for the rest.
    using namespace timeout_clock_test;
    int    timers = 0;
    double before = 0., next = 0.;
    on_a_loop_of_its_own([&](struct ev_loop *loop) {
        IdleTimer timer(1s);                // armed, from the loop's clock read then
        std::this_thread::sleep_for(300ms); // no pass: the loop's cached clock is 300 ms stale
        before = ev_clock_now();
        timer.setTimeout(200ms);
        timers = *ev_timer_count_addr(loop);
        next   = ev_timer_next(loop); // the deadline, on the loop's clock
    });
    ASSERT_EQ(timers, 1) << "the loop holds another timer: its earliest deadline is not this one's";
    EXPECT_GE(next - before, 0.2 - 1e-6) << "setTimeout() armed the timer from the loop's last pass, " << (before + 0.2 - next) * 1e3
                                         << " ms early";
}
