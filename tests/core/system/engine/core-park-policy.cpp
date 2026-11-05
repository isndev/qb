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
 *   - one park lasts a second at most, whatever the latency (QB-1227): `Main::stop()` wakes no parked core, so a
 *     longer park held a stop for its whole length, and `qb::duration::max()` returned at once and the core polled.
 *
 * The CPU/wall ratio is a diagnostic on a quiet, unconstrained host. A runnable core descheduled
 * by the OS can consume little CPU without ever parking; the ratio alone cannot prove the park.
 *
 * Every case runs the engine on the calling thread (`start(false)`) with process-global atoms and
 * ends through `kill()`, never through `Main::stop()`.
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
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

/// CPU time consumed by this process so far, every thread summed. Useful beside elapsed time
/// on a quiet host; descheduling can make a polling core look idle. `std::clock()` cannot be
/// used here because MSVC's returns wall time. A failed clock read also fails the test.
std::chrono::nanoseconds
process_cpu_time() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        const auto error = GetLastError();
        ADD_FAILURE() << "GetProcessTimes failed: " << error;
        return {};
    }
    auto to_ns = [](FILETIME const &ft) {
        const auto ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return std::chrono::nanoseconds{static_cast<std::int64_t>(ticks) * 100}; // 100 ns units
    };
    return to_ns(kernel) + to_ns(user);
#else
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
        const int error = errno;
        ADD_FAILURE() << "clock_gettime(CLOCK_PROCESS_CPUTIME_ID) failed: " << error;
        return {};
    }
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
    // checked together: the deadline was honoured (a cv park would have slept to `latency`),
    // while low process CPU is evidence of a park on a quiet, unconstrained host. The ratio is
    // not an unconditional oracle: an OS-descheduled polling core can also consume under half
    // the elapsed interval. A failed CPU clock read is reported separately by the helper.
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

// ===========================================================================
// One park lasts a second at most, whatever the latency (Huly QB-1227)
// ===========================================================================

std::atomic<std::uint64_t> g_passes{0};

/// Reads its core's pass count when the stop reaches it, then ends. With `far_timer` it also owns a qb-io timer an hour
/// away, so the idle core parks inside its event loop; without, on its mailbox's condition variable.
class StopCounter : public qb::Actor {
    bool                  _far_timer;
    std::shared_ptr<void> _timer; ///< the far timer; destroying it with the actor cancels it

public:
    explicit StopCounter(bool const far_timer)
        : _far_timer(far_timer) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SignalEvent>(*this);
        if (_far_timer)
            _timer = qb::io::async::scoped_callback([] {}, std::chrono::hours{1});
        co_return true;
    }
    void
    on(qb::SignalEvent const &) noexcept {
        g_passes.store(getCoreStats().loop_passes, std::memory_order_release);
        kill();
    }
};

struct park_run {
    std::uint64_t            passes = 0; ///< the core's passes from its start to the stop reaching it
    std::chrono::nanoseconds cpu{};      ///< the process's CPU time while the core idled
    std::chrono::nanoseconds took{};     ///< `Main::stop()` to `join()` returning
};

/// One core with `latency` and no idle spin -- it parks on its first idle pass -- left idle for `idle`, then stopped.
park_run
idle_then_stop(char const *label, qb::duration const latency, std::chrono::milliseconds const idle, bool const far_timer) {
    g_passes.store(0, std::memory_order_release);
    park_run r;
    qb::Main main;
    main.core(0).setLatency(latency).setIdleSpin(0us);
    main.addActor<StopCounter>(0, far_timer);
    main.start(true);
    auto const cpu_before = process_cpu_time();
    std::this_thread::sleep_for(idle);
    r.cpu              = process_cpu_time() - cpu_before;
    auto const stop_at = Clock::now();
    qb::Main::stop();
    main.join();
    r.took   = Clock::now() - stop_at;
    r.passes = g_passes.load(std::memory_order_acquire);
    EXPECT_FALSE(main.hasError());
    std::printf("        [park] latency %s, %lld ms idle: %llu passes, %lld us of CPU, the stop seen %lld ms after it was raised\n", label,
                static_cast<long long>(idle.count()), static_cast<unsigned long long>(r.passes),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(r.cpu).count()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(r.took).count()));
    return r;
}

TEST(CoreParkPolicy, ACoreGivenTheLongestLatencyParksASecondAtATime) {
    // `setLatency(qb::duration::max())`. Its park's end overflowed the clock, so the park returned at once and the core
    // polled -- 28 million passes a second on an M4; an untimed park instead would never have seen `Main::stop()`,
    // which wakes no parked core. One park lasts a second at most now.
    constexpr auto kIdle = 2500ms;
    auto const     r     = idle_then_stop("max", qb::duration::max(), kIdle, false);
    EXPECT_LE(r.passes, 12u) << r.passes << " passes in " << kIdle.count() << " ms idle: the core polled instead of parking";
    EXPECT_LT(r.cpu, kIdle / 4) << "the process ran " << std::chrono::duration_cast<std::chrono::milliseconds>(r.cpu).count()
                                << " ms of CPU over " << kIdle.count() << " ms idle: the core polled instead of parking";
    EXPECT_LT(r.took, 1500ms) << "the stop was seen " << std::chrono::duration_cast<std::chrono::milliseconds>(r.took).count()
                              << " ms after it was raised: a park outlasted a second";
}

TEST(CoreParkPolicy, ACoreParkedOnItsMailboxSeesAStopWithinASecondWhateverItsLatency) {
    // A latency of 20 s, no io watcher: the core parks on its condition variable, which `Main::stop()` does not notify. The
    // park held the stop for the whole latency; it lasts a second at most now.
    auto const r = idle_then_stop("20 s", 20s, 300ms, false);
    EXPECT_LT(r.took, 1500ms) << "the stop was seen " << std::chrono::duration_cast<std::chrono::milliseconds>(r.took).count()
                              << " ms after it was raised: the park waited out its latency";
}

TEST(CoreParkPolicy, ACoreParkedInItsLoopSeesAStopWithinASecondWhateverItsLatency) {
    // The same core owning a timer an hour away parks inside its event loop, its block capped by the same second. libev
    // bounds one blocking poll at 0.743 s of its own, except where it watches for time jumps with a timerfd (Linux,
    // `QB_EV_USE_TIMERFD`): there it blocked up to the latency, 20 s here, and some seventeen days given duration::max().
    auto const r = idle_then_stop("20 s, loop", 20s, 300ms, true);
    EXPECT_LT(r.cpu, 150ms) << "the process ran " << std::chrono::duration_cast<std::chrono::milliseconds>(r.cpu).count()
                            << " ms of CPU over 300 ms idle: the core polled instead of parking in its loop";
    EXPECT_LT(r.took, 1500ms) << "the stop was seen " << std::chrono::duration_cast<std::chrono::milliseconds>(r.took).count()
                              << " ms after it was raised: the loop park waited out its latency";
}

TEST(CoreParkPolicy, ALatencyUnderASecondStillParksItsWholeLatency) {
    // The positive control: the ceiling leaves a latency under a second alone. A core of 300 ms parks 300 ms at a time --
    // seven parks in 2.1 s; a shorter park shows as more passes.
    constexpr auto kIdle = 2100ms;
    auto const     r     = idle_then_stop("300 ms", 300ms, kIdle, false);
    EXPECT_GE(r.passes, 6u) << r.passes << " passes in " << kIdle.count() << " ms idle: the core parked longer than its latency";
    EXPECT_LE(r.passes, 16u) << r.passes << " passes in " << kIdle.count() << " ms idle: the core parked shorter than its latency";
    EXPECT_LT(r.took, 800ms) << "the stop was seen " << std::chrono::duration_cast<std::chrono::milliseconds>(r.took).count()
                             << " ms after it was raised, past a 300 ms park";
}

} // namespace core_park_policy_test
