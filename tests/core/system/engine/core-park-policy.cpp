/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/core-park-policy.cpp
 * @brief The park policy of a `latency > 0` core: WHEN it parks, and what it must never park over.
 *
 * `VirtualCore::__workflow__` decides at the end of every pass whether to poll again or to sleep in
 * `Mailbox::wait()`. Until 3.0 that decision was an event-COUNT credit refilled from the previous
 * pass, which parked a one-event-per-pass workload after two or three empty passes (every hop of a
 * ping-pong paid an OS park + wake, axis A of the qb-vs-others audit). It is a TIME floor now —
 * `CoreInitializer::setIdleSpin()`, default 50 µs — measured from the first idle pass, and an idle
 * pass is one that moved no event AND left nothing in the core's own pipe. What this file pins,
 * through the one instrument that tells "still polling" from "parked" apart from outside — a qb-io
 * timer, which fires at its delay while the core polls and at `latency` once it has parked:
 *
 *   - an actor pushing to ITSELF from its tick callback is delivered on the next pass, not after
 *     `latency`: the self-core pipe is not counted as activity, and 3.0 parked over it;
 *   - inside the idle-spin floor the core keeps polling, so a 100 ms timer fires at ~100 ms;
 *   - past the floor the core parks and that same timer fires at `latency` — the current contract,
 *     recorded here so the previous case cannot pass vacuously (axis J of the audit: a parked core
 *     does not consult io deadlines; if this starts firing early, the engine learned to, and the
 *     `setLatency()` documentation must move with it);
 *   - the configuration surface: default, per-core override, `Main`-wide fan-out, chaining.
 *
 * Every case runs the engine on the calling thread (`start(false)`) with process-global atoms and
 * ends through `kill()`, never through `Main::stop()`.
 */

#include <atomic>
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/main.h>

namespace core_park_policy_test {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::atomic<std::int64_t> g_pushed_at_ns{0};
std::atomic<std::int64_t> g_seen_at_ns{0};
std::atomic<std::int64_t> g_armed_at_ns{0};
std::atomic<std::int64_t> g_fired_at_ns{0};

std::int64_t
now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::chrono::nanoseconds
delta_ns(std::atomic<std::int64_t> const &from, std::atomic<std::int64_t> const &to) {
    return std::chrono::nanoseconds{to.load(std::memory_order_acquire) - from.load(std::memory_order_acquire)};
}

void
reset_atoms() {
    for (auto *a : {&g_pushed_at_ns, &g_seen_at_ns, &g_armed_at_ns, &g_fired_at_ns})
        a->store(0, std::memory_order_release);
}

struct Tick : qb::Event {};

/// Ticks `kPushOnTick` times, then pushes to ITSELF. Tick 1 lands on the pass right after init;
/// each later tick is one pass later — which, once the core parks, is one `latency` later. The
/// push therefore happens on a pass the policy has every reason to park after, and the delay it
/// measures is exactly the one 3.0 charged.
class SelfPusher
    : public qb::Actor
    , public qb::ICallback {
    static constexpr int kPushOnTick = 3;
    int                  _ticks{0};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (++_ticks == kPushOnTick) {
            g_pushed_at_ns.store(now_ns(), std::memory_order_release);
            push<Tick>(id());
        }
    }
    void
    on(Tick const &) {
        g_seen_at_ns.store(now_ns(), std::memory_order_release);
        kill();
    }
};

/// Arms one qb-io timer in `onInit()` and ends when it fires. The timer is the probe: it fires at
/// its delay while the core polls, and only when the park times out once the core has parked.
class TimerProbe : public qb::Actor {
    const qb::duration _delay;

public:
    explicit TimerProbe(qb::duration const delay)
        : _delay(delay) {}
    qb::io::async::task<bool>
    onInit() override {
        g_armed_at_ns.store(now_ns(), std::memory_order_release);
        qb::io::async::callback(
            [this] {
                g_fired_at_ns.store(now_ns(), std::memory_order_release);
                kill();
            },
            _delay);
        co_return true;
    }
};

TEST(CoreParkPolicy, SelfPushFromCallbackIsNotParkedOver) {
    reset_atoms();
    constexpr auto kLatency = 300ms;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(0us); // park on the first idle pass
    main.addActor<SelfPusher>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());

    ASSERT_NE(g_pushed_at_ns.load(std::memory_order_acquire), 0) << "the actor never pushed";
    ASSERT_NE(g_seen_at_ns.load(std::memory_order_acquire), 0) << "the self-push was never delivered";
    const auto delay = delta_ns(g_pushed_at_ns, g_seen_at_ns);
    EXPECT_LT(delay, kLatency / 3) << "a self-push must reach the next pass, not the next park timeout; delivered after "
                                   << std::chrono::duration_cast<std::chrono::milliseconds>(delay).count() << " ms";
}

TEST(CoreParkPolicy, InsideTheIdleSpinFloorTheCoreKeepsPolling) {
    reset_atoms();
    constexpr auto kLatency = 1s;
    constexpr auto kFloor   = 500ms;
    constexpr auto kTimer   = 100ms;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(kFloor);
    main.addActor<TimerProbe>(0, kTimer);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());

    ASSERT_NE(g_fired_at_ns.load(std::memory_order_acquire), 0) << "the timer never fired";
    const auto at = delta_ns(g_armed_at_ns, g_fired_at_ns);
    EXPECT_GE(at, kTimer - 20ms);
    EXPECT_LT(at, kFloor) << "the core parked before its idle-spin floor elapsed; the timer fired after "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(at).count() << " ms";
}

TEST(CoreParkPolicy, PastTheFloorTheCoreParksAndAnIoTimerWaitsForTheLatency) {
    // NOT a wish, a fact: a parked core does not consult qb-io's next deadline, so a timer armed
    // on it fires when the park times out. This is what makes the previous case load-bearing —
    // with the floor at zero the SAME timer fires at `latency`, so "fired at ~100 ms" above is the
    // floor doing its job, not parking never happening. If this ever fires early, the engine has
    // learned io deadlines; move this expectation and the `setLatency()` doc together.
    reset_atoms();
    constexpr auto kLatency = 700ms;
    constexpr auto kTimer   = 100ms;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(0us);
    main.addActor<TimerProbe>(0, kTimer);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());

    ASSERT_NE(g_fired_at_ns.load(std::memory_order_acquire), 0) << "the timer never fired";
    const auto at = delta_ns(g_armed_at_ns, g_fired_at_ns);
    EXPECT_GE(at, kLatency - 100ms) << "a parked core woke before its latency; the timer fired after "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(at).count() << " ms";
    EXPECT_LT(at, 3 * kLatency) << "and the park is bounded by the latency";
}

TEST(CoreParkPolicy, IdleSpinConfigurationSurface) {
    qb::Main main;
    EXPECT_EQ(qb::CoreInitializer::kDefaultIdleSpin, 50us);
    EXPECT_EQ(main.core(0).getIdleSpin(), qb::CoreInitializer::kDefaultIdleSpin);
    EXPECT_EQ(main.core(0).getLatency(), 0ns);

    auto &chained = main.core(0).setIdleSpin(20us).setLatency(1ms);
    EXPECT_EQ(&chained, &main.core(0)) << "setIdleSpin chains like setLatency";
    EXPECT_EQ(main.core(0).getIdleSpin(), 20us);
    EXPECT_EQ(main.core(0).getLatency(), 1ms);

    main.core(1).setIdleSpin(0us);
    main.setIdleSpin(3ms); // Main-wide fan-out overrides every per-core value
    EXPECT_EQ(main.core(0).getIdleSpin(), 3ms);
    EXPECT_EQ(main.core(1).getIdleSpin(), 3ms);
    main.core(1).setIdleSpin(); // the default argument restores the default
    EXPECT_EQ(main.core(1).getIdleSpin(), qb::CoreInitializer::kDefaultIdleSpin);
}

} // namespace core_park_policy_test
