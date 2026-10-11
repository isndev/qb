/**
 * @file qb/tests/io/unit/async/listener-io-cadence.cpp
 * @brief The io poll cadence of a non-blocking `listener::run()` (Huly QB-191).
 *
 * A loop that owns an fd used to poll its backend on EVERY non-blocking pass -- the syscall
 * (`epoll_wait(0)`, `kevent`, wepoll's IOCP wait) on every pass of every core with a socket,
 * finding nothing on nearly all of them. With an interval set, a pass polls only when the loop is
 * hot (the previous pass delivered something) or when the interval has elapsed since the last
 * poll; the passes in between run `ev_run` with `EVRUN_NOPOLL`, which keeps timers and pending
 * events exactly as before. The listener's own default is "every pass" (the 3.1 contract for a
 * program that drives `run(EVRUN_NOWAIT)` itself); a `VirtualCore` opts its core in.
 *
 * A case that asserts what a pass does INSIDE the interval holds only if its passes really ran
 * inside it, and a loaded host can stall a case past it: a 3 ms sleep that overran a 50 ms interval
 * on a macOS runner made the pass legitimately poll and deliver (Huly QB-975). So each such case is
 * an attempt that judges its own window -- on the cadence's own counter, `qb::tsc_ticks()` against
 * `io_poll_interval_ticks()`, the reading and the threshold the listener judges on -- asserts the
 * inside-the-interval outcome only when the window held, and is replayed otherwise, a bounded
 * number of times. And each attempt runs on a fresh thread, so on a fresh thread-local listener:
 * the cadence state (hot or cold, the last poll's stamp) outlives `listener::clear()`, and a case run
 * on the test's own thread opened its interval wherever the previous case had left it.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/system/time.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/raw_fd_fixture.h"

namespace listener_io_cadence_test {

using namespace std::chrono_literals;
using qb::io::async::listener;

class ListenerIoCadence : public ::testing::Test {
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
};

TEST_F(ListenerIoCadence, TheIntervalKnob) {
    EXPECT_EQ(listener::current.io_poll_interval_ticks(), 0u) << "the listener's own default is: poll on every pass";
    listener::current.set_io_poll_interval(1us);
    EXPECT_GT(listener::current.io_poll_interval_ticks(), 0u) << "one microsecond is a positive number of counter ticks";
    const auto one_us = listener::current.io_poll_interval_ticks();
    listener::current.set_io_poll_interval(1ms);
    EXPECT_GT(listener::current.io_poll_interval_ticks(), one_us * 500) << "a millisecond is at least 500x a microsecond in ticks";
    listener::current.set_io_poll_interval(qb::duration::zero());
    EXPECT_EQ(listener::current.io_poll_interval_ticks(), 0u);
}

using qb::io::test::Pipe;

TEST_F(ListenerIoCadence, EveryPassByDefault) {
    Pipe p;
    for (int i = 1; i <= 3; ++i) {
        p.put();
        listener::current.run(EVRUN_NOWAIT);
        EXPECT_EQ(p.hits, i) << "with no interval every non-blocking pass polls: one write, one pass, one delivery";
    }
}

// ---- The window of an attempt (Huly QB-975) ----------------------------------------------------

/// The interval of every cadence case below.
constexpr auto kInterval = 50ms;
/// How many attempts a case makes before it reports that none ran inside its interval. A transient
/// stall of the host spoils one attempt, not twenty in a row; a pass that never fits inside the
/// interval -- one that blocks, or waits for the interval -- spoils every one, and fails the case.
constexpr int kAttempts = 20;

/// The counter read on each side of the pass that started an interval -- a hot pass or a due one,
/// both of which stamp the poll they make: `before` is no later than the listener's stamp, `after`
/// no earlier.
struct Window {
    std::uint64_t before = 0;
    std::uint64_t after  = 0;
};

/// One non-blocking pass that stamps the interval's start, with the counter read around it.
Window
stamping_pass() {
    Window w;
    w.before = qb::tsc_ticks();
    listener::current.run(EVRUN_NOWAIT);
    w.after = qb::tsc_ticks();
    return w;
}

/// Sets the interval and runs the first pass of this thread's fresh listener, which polls -- hot at
/// construction, and due anyway against a stamp of zero -- so the interval starts there; it finds
/// nothing and leaves the loop cold.
Window
open_cold_interval() {
    listener::current.set_io_poll_interval(kInterval);
    return stamping_pass();
}

/// Whether every pass since `w` ran inside the interval, read after the last of them: the counter's
/// advance since `w.before` bounds what the listener measured from its stamp to its last judgement.
/// A sixteenth of the interval is kept spare, far above the few cycles an unserialised counter read
/// can move against the code around it; a counter read going backwards (a migration) is not inside.
bool
ran_inside(Window const &w) {
    const std::uint64_t interval = listener::current.io_poll_interval_ticks();
    const std::uint64_t now      = qb::tsc_ticks();
    return now >= w.before && now - w.before < interval - interval / 16;
}

/// Sleeps until the interval started at `w` has elapsed on the counter, a sixteenth spare: the next
/// non-blocking pass is due. Judged on the listener's counter, not on `sleep_for`'s clock, whose
/// rate the listener's calibration only approximates (Huly QB-925: judge on the mechanism's clock).
void
wait_until_due(Window const &w) {
    const std::uint64_t interval = listener::current.io_poll_interval_ticks();
    const std::uint64_t past     = interval + interval / 16;
    const auto          give_up  = qb::mono_now() + 100 * kInterval;
    for (;;) {
        const std::uint64_t now = qb::tsc_ticks();
        if (now >= w.after && now - w.after >= past)
            return;
        if (qb::mono_now() > give_up) {
            ADD_FAILURE() << "the counter did not cover the interval in a hundred times its length";
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
}

/// Runs `attempt` on a fresh thread, so on a fresh thread-local listener, and returns its result.
template <typename Attempt>
bool
on_fresh_thread(Attempt &attempt) {
    bool        result = false;
    std::thread t([&attempt, &result] {
        result = attempt();
        listener::current.clear();
    });
    t.join();
    return result;
}

/// Replays `attempt` -- which returns whether its passes ran inside the interval, i.e. whether its
/// verdict counts -- until one did, or one failed, at most `kAttempts` times.
template <typename Attempt>
void
replay_until_conclusive(Attempt attempt) {
    for (int i = 0; i < kAttempts; ++i)
        if (on_fresh_thread(attempt) || ::testing::Test::HasFailure())
            return;
    ADD_FAILURE() << "none of " << kAttempts << " attempts ran its passes inside the " << kInterval.count()
                  << " ms interval, on the cadence's own counter: a stall of the host does not cover that many, a pass "
                     "that never fits inside the interval does";
}

TEST_F(ListenerIoCadence, InsideTheIntervalThePollIsSkippedAndDueItRuns) {
    replay_until_conclusive([] {
        Pipe         p;
        const Window w = open_cold_interval();
        p.put();
        listener::current.run(EVRUN_NOWAIT);
        listener::current.run(EVRUN_NOWAIT);
        if (!ran_inside(w))
            return false; // the host stalled the attempt past the interval: a pass may have polled, legitimately
        EXPECT_EQ(p.hits, 0) << "cold and inside the interval: two passes ran, neither polled";
        wait_until_due(w);
        listener::current.run(EVRUN_NOWAIT);
        EXPECT_EQ(p.hits, 1) << "the interval elapsed: this pass polls and delivers";
        return true;
    });
}

TEST_F(ListenerIoCadence, ADeliveryKeepsTheLoopHotForTheNextPass) {
    replay_until_conclusive([] {
        Pipe         p;
        const Window opened = open_cold_interval();
        wait_until_due(opened);
        p.put();
        const Window due = stamping_pass(); // due: polls, delivers -> hot
        EXPECT_EQ(p.hits, 1) << "the interval elapsed: this pass polls and delivers";
        p.put();
        listener::current.run(EVRUN_NOWAIT); // hot: polls at once, whatever the interval
        const bool hot_inside = ran_inside(due);
        EXPECT_EQ(p.hits, 2) << "a burst stays at poll latency";
        const Window cold = stamping_pass(); // hot again after that delivery: polls, finds nothing -> cold
        EXPECT_EQ(p.hits, 2);
        p.put();
        listener::current.run(EVRUN_NOWAIT);
        if (!ran_inside(cold) || !hot_inside)
            return false; // past the interval the due rule polls too: neither verdict would count
        EXPECT_EQ(p.hits, 2) << "cold again: the byte waits for the interval";
        return true;
    });
}

static int cadence_timer_hits = 0;
static void
cadence_timer_cb(struct ev_loop *, ev_timer *, int) {
    ++cadence_timer_hits;
}

TEST_F(ListenerIoCadence, TimersAndPendingEventsIgnoreTheCadence) {
    replay_until_conclusive([] {
        Pipe         p;
        const Window w    = open_cold_interval(); // cold from here
        auto        *loop = static_cast<struct ev_loop *>(listener::current.loop());
        ev_timer     t;
        ev_timer_init(&t, &cadence_timer_cb, 0.001, 0.);
        ev_now_update(loop);
        ev_timer_start(loop, &t);
        cadence_timer_hits = 0;
        std::this_thread::sleep_for(3ms);
        p.put();
        listener::current.run(EVRUN_NOWAIT);
        EXPECT_EQ(cadence_timer_hits, 1) << "a pass that skips the poll still reifies and fires an expired timer";
        ev_timer_stop(loop, &t); // a no-op once the one-shot fired; never re-init a watcher the loop still holds
        ev_timer_init(&t, &cadence_timer_cb, 3600., 0.);
        ev_timer_start(loop, &t);
        ev_feed_event(loop, &t, EV_CUSTOM);
        listener::current.run(EVRUN_NOWAIT);
        const bool inside = ran_inside(w);
        EXPECT_EQ(cadence_timer_hits, 2) << "and still invokes a pending (fed) event";
        ev_timer_stop(loop, &t);
        if (!inside)
            return false; // the host stalled the attempt past the interval: a pass may have polled, legitimately
        EXPECT_EQ(p.hits, 0) << "while the readable fd waits for its interval: neither pass polled";
        return true;
    });
}

TEST_F(ListenerIoCadence, ABlockingPassAlwaysPolls) {
    // Delivered at once, not after the interval: an attempt the host stalled past the interval is
    // replayed, and a blocking pass that waited for the interval would land past it in every one.
    replay_until_conclusive([] {
        Pipe         p;
        const Window w = open_cold_interval();
        p.put();
        listener::current.run(EVRUN_ONCE); // a blocking pass: the poll is the wake, whatever the interval
        const bool inside = ran_inside(w);
        EXPECT_EQ(p.hits, 1) << "the blocking pass polled and delivered";
        return inside;
    });
}

} // namespace listener_io_cadence_test
