/**
 * @file qb/tests/io/unit/async/listener-timer-gate.cpp
 * @brief The pass that does not run the loop (Huly QB-190): a non-blocking `listener::run()` with
 *        nothing pending, no wake, no poll to make and no timer within reach calls `ev_run` not at all.
 *
 * What a core paid on every pass for one far timer -- a `sleep`, a retry, a keep-alive -- was
 * `ev_run` itself (~22 ns on g++): its clock read, the timer heap, the pending walk, to find
 * nothing. The listener now asks the loop inline (`ev_timer_count_addr`, `ev_timer_next`,
 * `ev_wake_pending_addr`, the pending counts) and judges "within reach" on the CPU's counter, so
 * such a pass costs a few loads; a timer within reach is judged on a real reading that the loop
 * is then handed (`ev_now_set`). The loop's iteration count is the witness of whether a pass ran
 * it, and every way the loop must still run is asserted here: a due timer, a pending event, a
 * wake from another thread, a byte on a socket at the cadence's next poll. The last case is the
 * consequence for the timer API: a loop that runs only when something is due has a stale clock
 * the rest of the time, so `event::timer` refreshes it before every arm -- the raw C arm, which
 * does not, is the negative control.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/io_cadence_window.h"
#include "../../shared/raw_fd_fixture.h"

namespace listener_timer_gate_test {

using namespace std::chrono_literals;
using qb::io::async::listener;
using qb::io::test::Pipe;

class ListenerTimerGate : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        listener::current.set_io_poll_interval(qb::duration::zero());
        listener::current.clear();
    }
    static struct ev_loop *
    loop() {
        return static_cast<struct ev_loop *>(listener::current.loop());
    }
    static unsigned
    iterations() {
        return ev_iteration(loop());
    }
    /// Runs `n` non-blocking passes; returns how many of them ran the loop.
    static unsigned
    loop_runs_over(int n) {
        const unsigned before = iterations();
        for (int i = 0; i < n; ++i)
            listener::current.run(EVRUN_NOWAIT);
        return iterations() - before;
    }
    /// Spins non-blocking passes for `d`, all of which should skip the loop: the loop's cached
    /// clock is left as stale as `d` afterwards.
    static void
    skip_for(qb::duration d) {
        const auto t0 = qb::mono_now();
        while (qb::mono_now() - t0 < d)
            listener::current.run(EVRUN_NOWAIT);
    }
};

/// A raw `ev_timer` on the current loop, armed the C way (with the refresh qb's own sites make).
struct RawTimer {
    ev_timer w{};
    int      hits = 0;
    explicit RawTimer(double after, bool refresh = true) {
        ev_timer_init(&w, &RawTimer::cb, after, 0.);
        w.data = this;
        if (refresh)
            ev_now_update(loop_of_current());
        ev_timer_start(loop_of_current(), &w);
    }
    ~RawTimer() {
        ev_timer_stop(loop_of_current(), &w);
    }
    static struct ev_loop *
    loop_of_current() {
        return static_cast<struct ev_loop *>(listener::current.loop());
    }
    static void
    cb(struct ev_loop *, ev_timer *w, int) {
        ++static_cast<RawTimer *>(w->data)->hits;
    }
};

TEST_F(ListenerTimerGate, AFarTimerCostsNoLoopPass) {
    RawTimer far_off(3600.); // not `far`: a Windows macro
    EXPECT_TRUE(listener::current.has_work()) << "a timer is work by the loop's own rule";
    EXPECT_EQ(loop_runs_over(1000), 0u) << "with one far timer, a thousand non-blocking passes ran the loop zero times";
    EXPECT_EQ(far_off.hits, 0);
}

TEST_F(ListenerTimerGate, ATimerWithinReachFiresOnTheFirstPassAfterItsDeadline) {
    // Judged pass by pass on the loop's own clock, the one the deadline is on (Huly QB-975): a bound
    // on how late the timer fires, read on the test's clock, is a bound on how long the host leaves
    // the thread alone around the deadline -- 2 ms here, which a loaded host did not keep (3.3 ms
    // under ASan in a parallel ctest). What the estimate must never do is let a pass that began past
    // the deadline go by without the real reading that fires the timer -- nor run the loop on a pass
    // that ended before it. The loop fires a timer whose deadline is strictly before its time, so a
    // pass that reads the deadline exactly may run it and leave the firing to the next one: on a
    // 100 ns clock (Windows) that pass is the rule, and neither count includes it.
    RawTimer     soon(0.005); // 5 ms
    const double deadline = ev_timer_next(loop());
    const auto   t0       = qb::mono_now();
    int          passes   = 0;
    int          missed   = 0; // passes that began past the deadline and left the timer unfired
    unsigned     early    = 0; // loop runs by passes that ended before the deadline
    while (soon.hits == 0 && qb::mono_now() - t0 < 1s) {
        const double   began = ev_clock_now();
        const unsigned runs  = iterations();
        listener::current.run(EVRUN_NOWAIT);
        const double ended = ev_clock_now();
        ++passes;
        if (ended < deadline)
            early += iterations() - runs;
        if (began > deadline && soon.hits == 0)
            ++missed;
    }
    EXPECT_EQ(soon.hits, 1) << "the timer fired";
    EXPECT_EQ(missed, 0) << "on the first pass that began past its deadline: the estimate only decides WHEN the real readings start (" << passes
                         << " passes)";
    EXPECT_EQ(early, 0u) << "every pass that ended before the deadline skipped the loop: the driver spun through the wait on the cheap "
                            "kind ("
                         << passes << " passes)";
}

TEST_F(ListenerTimerGate, ThePassBeforeTheDeadlineHandsItsReadingToTheLoop) {
    // The loop's realtime clock (`ev_now`) is derived from its monotonic one; a pass that fires a
    // timer with a supplied reading updates both, and `ev_now` moves with the wall clock exactly as
    // it did when the loop read the clock itself.
    RawTimer   soon(0.003);
    const auto rt0 = ev_now(loop());
    const auto t0  = qb::mono_now();
    while (soon.hits == 0 && qb::mono_now() - t0 < 1s)
        listener::current.run(EVRUN_NOWAIT);
    const double moved = ev_now(loop()) - rt0;
    EXPECT_EQ(soon.hits, 1);
    EXPECT_GE(moved, 0.0025) << "the loop's clock followed the supplied reading";
    EXPECT_LT(moved, 0.5);
}

TEST_F(ListenerTimerGate, TheLoopRunsForAPendingEvent) {
    RawTimer far_off(3600.); // not `far`: a Windows macro
    EXPECT_EQ(loop_runs_over(10), 0u);
    ev_feed_event(loop(), &far_off.w, EV_CUSTOM); // a fed event is pending
    EXPECT_EQ(loop_runs_over(1), 1u) << "a pending event runs the loop, whatever the timers say";
    EXPECT_EQ(far_off.hits, 1) << "and the pass invoked it";
    EXPECT_EQ(loop_runs_over(10), 0u) << "then the passes are cheap again";
}

TEST_F(ListenerTimerGate, TheLoopRunsForAWakeFromAnotherThread) {
    RawTimer far_off(3600.);      // not `far`: a Windows macro
    listener::current.arm_wake(); // the async watcher a producer ends a park with
    EXPECT_EQ(loop_runs_over(10), 0u) << "the armed wake alone is not work (it is unref'd) and not a pending wake";
    listener &me = listener::current;
    std::thread([&me] { me.wake(); }).join(); // between two passes, from another thread: the flag path
    EXPECT_EQ(loop_runs_over(1), 1u) << "a wake no pass has delivered runs the next one";
    EXPECT_EQ(loop_runs_over(10), 0u) << "and it was delivered: the passes are cheap again";
}

TEST_F(ListenerTimerGate, ASocketKeepsItsCadence) {
    // A far timer beside a quiet socket: the passes between two polls skip the loop entirely now
    // (before, they ran it with EVRUN_NOPOLL), and the byte is still seen at the cadence's next poll.
    // "Inside the interval" holds only if the host left the passes inside it: each attempt runs on a
    // fresh listener, judges its window on the cadence's counter and is replayed when it was stalled
    // past the interval (Huly QB-975, `io_cadence_window.h`).
    qb::io::test::replay_until_conclusive([] {
        Pipe                              p;
        RawTimer                          far_off(3600.);                                   // not `far`: a Windows macro
        const qb::io::test::CadenceWindow w       = qb::io::test::open_cold_interval(50ms); // polls, finds nothing, goes cold
        const unsigned                    skipped = loop_runs_over(100);
        p.put();
        const unsigned looked = loop_runs_over(1);
        if (!qb::io::test::ran_inside(w))
            return false; // the host stalled the attempt past the interval: a pass may have polled, legitimately
        EXPECT_EQ(skipped, 0u) << "cold and inside the interval: no poll, and no loop pass either";
        EXPECT_EQ(looked, 0u) << "the byte is not looked for before the interval";
        qb::io::test::wait_until_due(w);
        EXPECT_EQ(loop_runs_over(1), 1u) << "due: the pass polls";
        EXPECT_EQ(p.hits, 1) << "and delivers";
        return true;
    });
}

/// A handler for a registered `event::timer`. The callback counts.
struct TimerOwner {
    int hits = 0;
    void
    on(qb::io::async::event::timer &) {
        ++hits;
    }
};

TEST_F(ListenerTimerGate, AnEventTimerArmedAfterALongSkipStretchFiresOnTime) {
    // While the loop is not run its cached clock stands still. `event::timer::start` refreshes it
    // before arming (see event/timer.h), so a 20 ms timer armed after 30 ms of skipped passes fires
    // ~20 ms after the arm -- and the raw C arm without the refresh, the negative control, fires it
    // early: its deadline was computed against a clock 30 ms behind, so it is already past.
    RawTimer far_off(3600.); // not `far`: a Windows macro
    {
        // The control's verdict is the first pass after the arm firing the timer, read on the loop's
        // clock (Huly QB-975): it was a 12 ms bound on the test's clock, which a host that holds the
        // thread for one scheduler quantum breaks. That pass counts only if it began less than the
        // timer's 20 ms after the arm -- past that, a refreshed arm would fire on it too -- and an
        // attempt the host stalled that long is replayed.
        bool conclusive = false;
        for (int attempt = 0; attempt < qb::io::test::kCadenceAttempts && !conclusive && !HasFailure(); ++attempt) {
            ev_timer raw{};
            int      hits = 0;
            skip_for(30ms);
            EXPECT_EQ(loop_runs_over(1), 0u) << "the stretch skipped the loop, so its clock is 30 ms stale";
            ev_timer_init(&raw, [](struct ev_loop *, ev_timer *w, int) { ++*static_cast<int *>(w->data); }, 0.02, 0.);
            raw.data              = &hits;
            const double armed_at = ev_clock_now();
            ev_timer_start(loop(), &raw);        // the C arm: no refresh, against the stale clock
            listener::current.run(EVRUN_NOWAIT); // the first pass after the arm
            const double judged_by = ev_clock_now();
            ev_timer_stop(loop(), &raw);
            EXPECT_EQ(hits, 1) << "negative control: the raw arm against the stale clock fired the 20 ms timer on the first pass after it";
            conclusive = judged_by - armed_at < 0.02 - 0.02 / 16;
        }
        EXPECT_TRUE(conclusive) << "no attempt reached its first pass inside the timer's 20 ms after the arm";
    }
    {
        TimerOwner owner;
        auto      &t  = listener::current.registerEvent<qb::io::async::event::timer>(owner);
        const auto t0 = qb::mono_now();
        skip_for(30ms);
        t.start(0.02); // the qb wrapper: the loop's clock is refreshed first
        while (owner.hits == 0 && qb::mono_now() - t0 < 1s)
            listener::current.run(EVRUN_NOWAIT);
        const auto after_arm = qb::mono_now() - t0 - 30ms;
        EXPECT_EQ(owner.hits, 1);
        EXPECT_GE(after_arm, 18ms) << "`event::timer::start` refreshed the clock: the timer fired 20 ms after the arm, not before";
        EXPECT_LT(after_arm, 200ms);
        listener::current.unregisterEvent(t._interface);
    }
}

} // namespace listener_timer_gate_test
