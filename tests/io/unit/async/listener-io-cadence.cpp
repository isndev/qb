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
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>

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

TEST_F(ListenerIoCadence, InsideTheIntervalThePollIsSkippedAndDueItRuns) {
    Pipe p;
    listener::current.set_io_poll_interval(50ms);
    listener::current.run(EVRUN_NOWAIT); // the first pass after construction is hot: it polls, finds nothing, and goes cold
    p.put();
    listener::current.run(EVRUN_NOWAIT);
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(p.hits, 0) << "cold and inside the interval: two passes ran, neither polled";
    std::this_thread::sleep_for(60ms);
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(p.hits, 1) << "the interval elapsed: this pass polls and delivers";
}

TEST_F(ListenerIoCadence, ADeliveryKeepsTheLoopHotForTheNextPass) {
    Pipe p;
    listener::current.set_io_poll_interval(50ms);
    listener::current.run(EVRUN_NOWAIT); // polls (hot at start), nothing: cold
    std::this_thread::sleep_for(60ms);
    p.put();
    listener::current.run(EVRUN_NOWAIT); // due: polls, delivers -> hot
    ASSERT_EQ(p.hits, 1);
    p.put();
    listener::current.run(EVRUN_NOWAIT); // hot: polls at once, whatever the interval
    EXPECT_EQ(p.hits, 2) << "a burst stays at poll latency";
    listener::current.run(EVRUN_NOWAIT); // hot again after that delivery: polls, finds nothing -> cold
    p.put();
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(p.hits, 2) << "cold again: the byte waits for the interval";
}

static int cadence_timer_hits = 0;
static void
cadence_timer_cb(struct ev_loop *, ev_timer *, int) {
    ++cadence_timer_hits;
}

TEST_F(ListenerIoCadence, TimersAndPendingEventsIgnoreTheCadence) {
    Pipe p;
    listener::current.set_io_poll_interval(50ms);
    listener::current.run(EVRUN_NOWAIT); // cold from here
    auto    *loop = static_cast<struct ev_loop *>(listener::current.loop());
    ev_timer t;
    ev_timer_init(&t, &cadence_timer_cb, 0.001, 0.);
    ev_now_update(loop);
    ev_timer_start(loop, &t);
    cadence_timer_hits = 0;
    std::this_thread::sleep_for(3ms);
    p.put();
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(cadence_timer_hits, 1) << "a pass that skips the poll still reifies and fires an expired timer";
    EXPECT_EQ(p.hits, 0) << "while the readable fd waits for its interval";
    ev_timer_init(&t, &cadence_timer_cb, 3600., 0.);
    ev_timer_start(loop, &t);
    ev_feed_event(loop, &t, EV_CUSTOM);
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(cadence_timer_hits, 2) << "and still invokes a pending (fed) event";
    ev_timer_stop(loop, &t);
}

TEST_F(ListenerIoCadence, ABlockingPassAlwaysPolls) {
    Pipe p;
    listener::current.set_io_poll_interval(50ms);
    listener::current.run(EVRUN_NOWAIT); // cold
    p.put();
    const auto t0 = qb::mono_now();
    listener::current.run(EVRUN_ONCE); // a blocking pass: the poll is the wake, whatever the interval
    EXPECT_EQ(p.hits, 1);
    EXPECT_LT(qb::mono_now() - t0, 40ms) << "delivered at once, not after the interval";
}

} // namespace listener_io_cadence_test
