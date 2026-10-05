/**
 * @file qb/tests/io/unit/async/listener-dispatch-timing.cpp
 * @brief `listener::set_dispatch_timing()`: a bare listener that times its watcher dispatch (Huly QB-165).
 *
 * A listener driven outside qb-core has no core pass to time; what it can time is the dispatch --
 * the callbacks one libev iteration runs -- which is where a blocking callback stalls it. Timing
 * swaps libev's own `ev_invoke_pending` for one that reads the clock around it, and turning it off
 * puts libev's back. What this file pins:
 *
 *   - a listener that never timed reports zero;
 *   - a timed listener shows a 30 ms callback as its longest and recent-worst dispatch;
 *   - once timing is off, a slow callback leaves the stats as they were.
 *
 * Each case runs on its own thread: the listener is thread-local, so every case starts from a
 * fresh one whatever ran before it in this binary.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>

namespace listener_dispatch_timing_test {

using namespace std::chrono_literals;
using qb::io::async::listener;

/// Run `body` on a fresh thread (a fresh thread-local listener) and wait for it.
template <typename Body>
void
on_fresh_thread(Body body) {
    std::thread t([&body] {
        body();
        listener::current.clear();
    });
    t.join();
}

/// One callback after 1 ms that blocks for `stall`, the loop pumped until it ran.
void
run_one_slow_callback(qb::duration const stall) {
    bool fired = false;
    qb::io::async::callback(
        [&fired, stall] {
            std::this_thread::sleep_for(stall);
            fired = true;
        },
        1ms);
    const auto deadline = qb::mono_now() + 5s;
    while (!fired && qb::mono_now() < deadline)
        qb::io::async::run(EVRUN_ONCE);
    ASSERT_TRUE(fired);
}

TEST(ListenerDispatchTiming, AListenerThatNeverTimedReportsZero) {
    on_fresh_thread([] {
        run_one_slow_callback(2ms);
        const auto s = listener::current.dispatch_timing();
        EXPECT_EQ(s.count, 0u);
        EXPECT_EQ(s.total, qb::duration::zero());
        EXPECT_EQ(s.max, qb::duration::zero());
        EXPECT_EQ(s.recent_max, qb::duration::zero());
    });
}

TEST(ListenerDispatchTiming, ATimedListenerShowsASlowCallback) {
    on_fresh_thread([] {
        listener::current.set_dispatch_timing(true);
        run_one_slow_callback(30ms);
        const auto s = listener::current.dispatch_timing();
        EXPECT_GE(s.count, 1u);
        EXPECT_GE(s.max, 30ms);
        EXPECT_GE(s.recent_max, 30ms); // read within a second of the callback
        EXPECT_GE(s.total, 30ms);
        listener::current.set_dispatch_timing(false);
    });
}

TEST(ListenerDispatchTiming, OffAgainASlowCallbackLeavesTheStatsAlone) {
    on_fresh_thread([] {
        listener::current.set_dispatch_timing(true);
        run_one_slow_callback(2ms);
        const auto before = listener::current.dispatch_timing();
        ASSERT_GE(before.count, 1u);
        ASSERT_LT(before.max, 30ms);

        listener::current.set_dispatch_timing(false); // libev's own dispatch again
        run_one_slow_callback(30ms);
        const auto after = listener::current.dispatch_timing();
        EXPECT_EQ(after.count, before.count);
        EXPECT_EQ(after.total, before.total);
        EXPECT_EQ(after.max, before.max);
    });
}

} // namespace listener_dispatch_timing_test
