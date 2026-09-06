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
 *   - past the floor the core parks INSIDE ITS EVENT LOOP — it owns a timer, so `Mailbox::wait()`
 *     takes the `listener &` form and blocks in `ev_run(EVRUN_ONCE)` under a `latency` cap — and
 *     that same timer fires at its delay, not at `latency`: the loop park honours io deadlines
 *     (axis N of the audit; until 3.2 the core parked on a condition variable and a parked core
 *     consulted no io deadline, so this timer fired at the park timeout). That the core PARKED,
 *     rather than polled its way to the deadline, is measured: the process burns almost no CPU
 *     time across the wait, where a polling core burns the whole of it;
 *   - the cap holds: a core parked in its loop over a far deadline wakes at `latency` and sees a
 *     `Main::stop()` — without the cap timer the poll would sleep to the far deadline;
 *   - the configuration surface: default, per-core override, `Main`-wide fan-out, chaining.
 *
 * Every case runs the engine on the calling thread (`start(false)`) with process-global atoms and
 * ends through `kill()`, never through `Main::stop()`.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/main.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace core_park_policy_test {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/// CPU time consumed by this process so far, every thread summed. The instrument that tells a
/// parked core from a polling one: both reach the same deadline at the same wall-clock time,
/// only one of them burns the interval. `std::clock()` cannot be it — MSVC's returns wall time.
std::chrono::nanoseconds
process_cpu_time() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return {};
    auto to_ns = [](FILETIME const &ft) {
        const auto ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return std::chrono::nanoseconds{static_cast<std::int64_t>(ticks) * 100}; // 100 ns units
    };
    return to_ns(kernel) + to_ns(user);
#else
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
        return {};
    return std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec};
#endif
}

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
/// its delay whether the core polls or has parked inside its loop, because a loop park computes
/// its wait from the loop's own timer deadlines -- the pre-3.2 mailbox park could not, and fired
/// it only when the park timed out.
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

TEST(CoreParkPolicy, PastTheFloorTheCoreParksInsideItsLoopAndAnIoTimerFiresAtItsDelay) {
    // The core owns a timer, so past the floor it parks in `ev_run(EVRUN_ONCE)` rather than on
    // its condition variable, and the timer fires at 100 ms under a 700 ms latency. Two things are
    // asserted, because either alone passes vacuously: the deadline was honoured (a cv park would
    // have slept to `latency`) AND the core was asleep while it waited (a floor that never parks
    // would also fire on time). The second is CPU time: this process IS the core (`start(false)`),
    // so across a 100 ms wait a polling core charges ~100 ms and a parked one a few hundred µs.
    // Loose bound — a sanitizer, a loaded host and a coarse Windows tick all inflate it — but a
    // core that polled to the deadline lands at 100%, and no amount of noise turns that into 50.
    reset_atoms();
    constexpr auto kLatency = 700ms;
    constexpr auto kTimer   = 100ms;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(0us);
    main.addActor<TimerProbe>(0, kTimer);
    const auto cpu_before = process_cpu_time();
    main.start(false);
    main.join();
    const auto cpu_spent = process_cpu_time() - cpu_before;
    EXPECT_FALSE(main.hasError());

    ASSERT_NE(g_fired_at_ns.load(std::memory_order_acquire), 0) << "the timer never fired";
    const auto at = delta_ns(g_armed_at_ns, g_fired_at_ns);
    EXPECT_GE(at, kTimer - 20ms);
    EXPECT_LT(at, kLatency - 100ms) << "the timer waited for the park timeout: the core parked over its io deadline; fired after "
                                    << std::chrono::duration_cast<std::chrono::milliseconds>(at).count() << " ms";
    EXPECT_LT(cpu_spent, at / 2) << "the core polled its way to the deadline instead of parking: "
                                 << std::chrono::duration_cast<std::chrono::microseconds>(cpu_spent).count() << " us of CPU across a "
                                 << std::chrono::duration_cast<std::chrono::microseconds>(at).count() << " us wait";
}

TEST(CoreParkPolicy, ALoopParkIsCappedByTheLatencyAndSeesStop) {
    // A core parked in its loop over a deadline ten seconds away must still wake at `latency`,
    // because that is when it looks at `Main::stop()`, at a peer's shutdown, at anything that is
    // not a watcher. The cap is a one-shot timer the park arms around the blocking pass; without
    // it this join would take the ten seconds.
    reset_atoms();
    constexpr auto kLatency = 300ms;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(0us);
    main.addActor<TimerProbe>(0, qb::duration{10s});
    main.start(true);
    std::this_thread::sleep_for(100ms); // past the floor: the core is in its loop park by now
    const auto stop_at = Clock::now();
    qb::Main::stop();
    main.join();
    const auto took = Clock::now() - stop_at;
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_fired_at_ns.load(std::memory_order_acquire), 0) << "the ten-second timer fired";
    EXPECT_LT(took, 5 * kLatency) << "stop() waited on the io deadline, not on the latency cap: joined after "
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms";
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
