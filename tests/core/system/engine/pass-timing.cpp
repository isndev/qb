/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/pass-timing.cpp
 * @brief `CoreInitializer::setPassTiming()`: a core that times its passes, the park excluded (Huly QB-165).
 *
 * Before 3.3 nothing measured how long a pass took, so a handler that blocked its core -- a
 * synchronous call, a long loop -- showed nowhere but in everyone's latency. What this file pins:
 *
 *   - a timed core counts every completed pass (a snapshot taken during pass N reads N - 1), and a
 *     40 ms stall inside a handler is the longest pass, the recent worst case and part of the sum,
 *     while the pass after it is short again;
 *   - the park is not a pass: a timed core that sleeps 200 ms in a coroutine, parking between
 *     wakes, accounts for a small fraction of that wall time;
 *   - an untimed core -- every core's default -- reports zero.
 *
 * Snapshots are written to globals on the core's thread and read after `join()`, which orders them.
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace pass_timing_test {

using namespace std::chrono_literals;

constexpr auto kStall = 40ms;

// --- a stall inside a handler -------------------------------------------------------------------

qb::CoreStats g_after_stall{};

class Staller
    : public qb::Actor
    , public qb::ICallback {
    int _tick = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        ++_tick;
        if (_tick == 5)
            std::this_thread::sleep_for(kStall); // the blocking handler a pass gauge exists to show
        if (_tick == 10) {
            g_after_stall = getCoreStats();
            kill();
        }
    }
};

TEST(PassTiming, ATimedCoreCountsEveryPassAndShowsAStall) {
    qb::Main engine;
    engine.core(0).setLatency(qb::duration::zero()).setPassTiming(true);
    engine.addActor<Staller>(0);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());

    const auto &t = g_after_stall.pass_time;
    // Read during pass N: passes 1 .. N - 1 are complete, each timed once.
    EXPECT_EQ(t.count, g_after_stall.loop_passes - 1);
    EXPECT_GE(t.max, kStall);
    EXPECT_GE(t.recent_max, kStall); // read well within a second of the stall
    EXPECT_GE(t.total, kStall);
    EXPECT_LT(t.last, kStall); // the pass before the snapshot did not stall
    EXPECT_LE(t.last, t.max);
}

// --- the park is not a pass -----------------------------------------------------------------------

qb::CoreStats g_after_sleep{};
qb::duration  g_slept{};

struct Woke : qb::Event {};

class Sleeper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Woke>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start = qb::mono_now();
            co_await ctx.sleep(200ms);
            g_slept = qb::mono_now() - start;
            ctx.push<Woke>();
        });
        co_return true;
    }
    void
    on(Woke const &) {
        g_after_sleep = getCoreStats();
        kill();
    }
};

TEST(PassTiming, TheParkIsNotTimed) {
    qb::Main engine;
    engine.core(0).setLatency(std::chrono::milliseconds{1}).setPassTiming(true); // parks between wakes
    engine.addActor<Sleeper>(0);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());

    ASSERT_GE(g_slept, 200ms);
    const auto &t = g_after_sleep.pass_time;
    EXPECT_GT(t.count, 0u);
    // Two hundred milliseconds of wall time, nearly all of it parked: the passes account for a
    // small part of it (the bound is loose on purpose -- a loaded host stretches passes, not parks).
    EXPECT_LT(t.total, g_slept / 2);
}

// --- an untimed core reports zero -----------------------------------------------------------------

qb::CoreStats g_untimed{};

class Counter
    : public qb::Actor
    , public qb::ICallback {
    int _tick = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (++_tick == 10) {
            g_untimed = getCoreStats();
            kill();
        }
    }
};

TEST(PassTiming, AnUntimedCoreReportsZero) {
    qb::Main engine;
    engine.addActor<Counter>(0); // no setPassTiming: every core's default
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());

    EXPECT_GE(g_untimed.loop_passes, 10u);
    const auto &t = g_untimed.pass_time;
    EXPECT_EQ(t.count, 0u);
    EXPECT_EQ(t.total, qb::duration::zero());
    EXPECT_EQ(t.last, qb::duration::zero());
    EXPECT_EQ(t.max, qb::duration::zero());
    EXPECT_EQ(t.recent_max, qb::duration::zero());
}

} // namespace pass_timing_test
