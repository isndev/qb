/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/listener-has-work.cpp
 * @brief `listener::has_work()` and the libev-pass gate inside `listener::run()`.
 *
 * A `VirtualCore` pumps qb-io on every loop pass — and a pass with nothing to deliver is not
 * free: `ev_run(EVRUN_NOWAIT)` still costs a `backend_poll(0)` and two clock reads, ~300–380 ns
 * measured. The old core gate was `has_coro_scheduler() || size() || has_deferred()`, whose
 * first term is TRUE for the rest of a core's life once any actor has spawned one coroutine,
 * so every such core paid that on every tick. The scheduler had to be in the gate because
 * `size()` counts only handlers registered THROUGH the listener and cannot see a raw
 * `ev_timer_start` from a coroutine awaiter — the loop could hold a live timer while the
 * listener reported zero.
 *
 * `has_work()` asks the loop itself — `ev_active_count()` / `ev_pending_count()` — plus the
 * deferred queue and the coroutine ready queue, and `run()` skips the libev pass when the
 * loop has neither an active nor a pending watcher. What this file pins, in both polarities:
 *
 *   - an idle loop reports no work and `run(EVRUN_NOWAIT)` does NOT enter libev (its
 *     iteration counter is the witness: `ev_iteration()` advances on every pass ev_run makes);
 *   - a listener-registered timer, a RAW libev timer the listener never counted, a `defer()`,
 *     and a ready coroutine are each reported, each delivered, and each stop being reported
 *     once delivered;
 *   - an idle coroutine scheduler is NOT work — the exact false positive the old gate had;
 *   - a `defer()` issued from inside a deferred callback (the re-armed 0-delay one-shot path)
 *     keeps being reported until it has run, and the re-armed hook is not left active after.
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

namespace listener_has_work_test {

using namespace std::chrono_literals;
using qb::io::async::listener;

class ListenerHasWork : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler())
            listener::current.reset_coro_scheduler();
        listener::current.clear();
    }

    static struct ev_loop *
    raw_loop() {
        return static_cast<struct ev_loop *>(listener::current.loop());
    }
    static unsigned
    iterations() {
        return ev_iteration(raw_loop());
    }
    /// Pump NOWAIT turns until the listener reports nothing left, bounded.
    static bool
    drain(int max_turns = 1000) {
        for (int i = 0; i < max_turns; ++i) {
            if (!listener::current.has_work())
                return true;
            listener::current.run(EVRUN_NOWAIT);
            if (!listener::current.has_work())
                return true;
            std::this_thread::sleep_for(100us); // let a 0-delay one-shot cross `mn_now`
        }
        return false;
    }
};

// ---- the idle loop ----------------------------------------------------------------------------

TEST_F(ListenerHasWork, IdleLoopReportsNoWorkAndRunSkipsLibev) {
    ASSERT_EQ(listener::current.size(), 0u);
    EXPECT_FALSE(listener::current.has_work());
    const unsigned before = iterations();
    for (int i = 0; i < 100; ++i)
        listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(iterations(), before) << "an idle NOWAIT turn must not enter ev_run";
    EXPECT_EQ(listener::current.nb_invoked_event(), 0u);
}

TEST_F(ListenerHasWork, IdleCoroutineSchedulerIsNotWork) {
    (void) listener::current.coro_scheduler();
    ASSERT_TRUE(listener::current.has_coro_scheduler());
    EXPECT_FALSE(listener::current.has_work())
        << "a scheduler that exists but has nothing ready is the old gate's false positive";
    const unsigned before = iterations();
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(iterations(), before);
}

// ---- each source of work, reported then retired ----------------------------------------------

TEST_F(ListenerHasWork, RegisteredTimerIsWorkUntilItFires) {
    std::atomic<int> fired{0};
    qb::io::async::callback([&] { ++fired; }, 1ms);
    ASSERT_EQ(listener::current.size(), 1u);
    EXPECT_TRUE(listener::current.has_work());
    const unsigned before = iterations();
    ASSERT_TRUE(drain());
    EXPECT_GT(iterations(), before) << "work reported means ev_run really ran";
    EXPECT_EQ(fired.load(), 1);
    EXPECT_FALSE(listener::current.has_work());
}

TEST_F(ListenerHasWork, RawLibevTimerTheListenerNeverCountedIsStillWork) {
    // What a coroutine awaiter does: ev_timer_start straight on the loop. `size()` stays 0.
    std::atomic<int> fired{0};
    ev_timer         t;
    ev_timer_init(
        &t, [](struct ev_loop *, ev_timer *w, int) { ++*static_cast<std::atomic<int> *>(w->data); }, 0.001,
        0.0);
    t.data = &fired;
    ev_timer_start(raw_loop(), &t);
    EXPECT_EQ(listener::current.size(), 0u) << "the listener does not know about it";
    EXPECT_TRUE(listener::current.has_work()) << "the loop does";
    ASSERT_TRUE(drain());
    EXPECT_EQ(fired.load(), 1);
    EXPECT_FALSE(listener::current.has_work()) << "a fired one-shot is no longer active";
    ev_timer_stop(raw_loop(), &t);
}

TEST_F(ListenerHasWork, DeferredCallbackIsWorkUntilDrained) {
    std::atomic<int> ran{0};
    listener::current.defer([&] { ++ran; });
    EXPECT_TRUE(listener::current.has_deferred());
    EXPECT_TRUE(listener::current.has_work());
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(ran.load(), 1);
    EXPECT_FALSE(listener::current.has_deferred());
    EXPECT_FALSE(listener::current.has_work());
}

TEST_F(ListenerHasWork, DeferFromWithinDeferIsReportedUntilItHasRun) {
    // The outer callback runs from the in-loop hook (`_defer_wake`, a fed event the loop
    // reports as pending); its re-defer is excluded from that pass and re-arms the hook
    // as a 0-delay one-shot, which the loop reports as ACTIVE. Whichever drain ends up
    // running the inner callback, the listener must keep reporting work until it has.
    std::atomic<int> outer{0}, inner{0};
    listener::current.defer([&] {
        ++outer;
        listener::current.defer([&] { ++inner; });
    });
    EXPECT_TRUE(listener::current.has_work());
    ASSERT_TRUE(drain());
    EXPECT_EQ(outer.load(), 1);
    EXPECT_EQ(inner.load(), 1);
    EXPECT_FALSE(listener::current.has_deferred());
    EXPECT_FALSE(listener::current.has_work()) << "the re-armed hook must not be left active";
}

TEST_F(ListenerHasWork, ReadyCoroutineIsWorkAndSleepingOneStaysWork) {
    std::atomic<int> stage{0};
    auto             body = [&]() -> qb::io::async::task<void> {
        stage = 1;
        co_await qb::io::async::sleep(1ms); // a raw ev_timer, invisible to size()
        stage = 2;
    };
    listener::current.coro_scheduler().spawn(body());
    EXPECT_TRUE(listener::current.has_work()) << "spawned = on the ready queue";
    listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(stage.load(), 1);
    EXPECT_EQ(listener::current.size(), 0u);
    EXPECT_TRUE(listener::current.has_work()) << "suspended on a timer the loop holds";
    ASSERT_TRUE(drain());
    EXPECT_EQ(stage.load(), 2);
    EXPECT_FALSE(listener::current.has_work());
}

} // namespace listener_has_work_test
