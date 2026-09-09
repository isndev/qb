/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/core-park-wake.cpp
 * @brief What WAKES a parked `latency > 0` core, through each of its two parks.
 *
 * `core-park-policy.cpp` pins when a core parks. This file pins that every source of work ends
 * the park promptly, and it does so against a latency chosen so that a wake which fell through
 * to the timeout would be unmistakable — a missed wake is `latency` late, and every bound here
 * sits well under it. A core parks in one of two places (`Mailbox::wait()` in `Main.h`):
 *
 *   - INSIDE ITS EVENT LOOP when it owns active qb-io watchers — `ev_run(EVRUN_ONCE)` under a
 *     `latency` cap. Two things must end that park: io readiness (a socket becoming readable
 *     wakes the core at poll latency, where until 3.2 it waited for the park timeout — the
 *     qb-vs-others audit measured p50 1.1 ms at 100 µs latency, 15 ms at 10 ms, against 19 µs
 *     while polling; axis N) and a producer's `notify()` — which cannot signal a condition
 *     variable the core is not waiting on, so it sends the loop's `ev_async` instead;
 *   - ON ITS MAILBOX CONDITION VARIABLE when it owns none, where `notify()` signals the cv.
 *
 * The producer chooses between the two under the mailbox mutex, from the consumer's published
 * park state; each case below plants the core in one park and pushes through the matching path.
 * The socket case also measures that the core PARKED across its rounds, by process CPU time —
 * a core that polled between requests would answer just as fast.
 *
 * Every latency here is large next to its bound and next to a sanitizer's overhead: these are
 * order-of-magnitude assertions, not timing ones.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/socket.h>
#include <qb/main.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#if defined(__linux__) && EV_USE_EPOLL_PWAIT2
#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace core_park_wake_test {

using namespace std::chrono_literals;
using namespace qb::io;
using Clock = std::chrono::steady_clock;

std::atomic<std::int64_t>  g_pushed_at_ns{0};
std::atomic<std::int64_t>  g_seen_at_ns{0};
std::atomic<std::uint16_t> g_port{0};

std::int64_t
now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void
reset_atoms() {
    g_pushed_at_ns.store(0, std::memory_order_release);
    g_seen_at_ns.store(0, std::memory_order_release);
    g_port.store(0, std::memory_order_release);
}

/// CPU time consumed by this process so far, every thread summed — the instrument that tells a
/// parked core from a polling one. `std::clock()` cannot be it: MSVC's returns wall time.
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

// ---- a TCP echo server, the io half ------------------------------------------------------------

class EchoActor;

class EchoSession : public use<EchoSession>::tcp::client<EchoActor> {
public:
    using Protocol = qb::protocol::text::command<EchoSession>;
    explicit EchoSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        *this << msg.text << Protocol::end;
    }
};

/// Listens on an ephemeral loopback port, publishes it, echoes every line. Its listening socket
/// is an active io watcher, so the core it lives on parks in its loop.
class EchoActor
    : public qb::Actor
    , public use<EchoActor>::tcp::server<EchoSession> {
public:
    qb::io::async::task<bool>
    onInit() override {
        if (transport().listen_v4(0, "127.0.0.1") != 0)
            co_return false;
        start();
        g_port.store(transport().local_endpoint().port(), std::memory_order_release);
        co_return true;
    }
    void
    on(IOSession &) {}
};

// ---- a cross-core push, the mailbox half --------------------------------------------------------

struct Ping : qb::Event {};

/// Waits for one `Ping`, stamps its arrival, dies. With `hold_loop` it also arms a far timer, so
/// its core owns an io watcher and parks in the loop rather than on the condition variable.
class Target : public qb::Actor {
    const bool _hold_loop;

public:
    explicit Target(bool const hold_loop)
        : _hold_loop(hold_loop) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        if (_hold_loop)
            qb::io::async::callback([] {}, 60s);
        co_return true;
    }
    void
    on(Ping const &) {
        g_seen_at_ns.store(now_ns(), std::memory_order_release);
        kill();
    }
};

/// Lives on a spinning core, and once `delay` has passed since its first tick — long past the
/// target core's idle-spin floor, so the target has parked — pushes one `Ping` and dies.
class Pusher
    : public qb::Actor
    , public qb::ICallback {
    const qb::ActorId  _target;
    const qb::duration _delay;
    Clock::time_point  _first_tick{};

public:
    Pusher(qb::ActorId const target, qb::duration const delay)
        : _target(target)
        , _delay(delay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        const auto now = Clock::now();
        if (_first_tick == Clock::time_point{})
            _first_tick = now;
        if (now - _first_tick < _delay)
            return;
        g_pushed_at_ns.store(now_ns(), std::memory_order_release);
        push<Ping>(_target);
        kill();
    }
};

// ---- a timer under a millisecond, the granularity of the park's wait ---------------------------

std::atomic<std::int64_t> g_best_lateness_ns{-1};
std::atomic<unsigned int> g_backend{0};

/// Arms one 200 µs callback from inside the previous one, twenty times, and keeps the BEST
/// lateness (fired-at minus due-at). Its core has nothing else to do, so every wait is a park in
/// the loop bounded by that timer -- the shape of a keep-alive, a retry or a poll under a
/// millisecond on an otherwise idle server core.
class SubMsTimerActor : public qb::Actor {
    static constexpr int  kRounds = 20;
    static constexpr auto kDelay  = 200us;
    int                   _round  = 0;
    Clock::time_point     _due{};

    void
    arm() {
        _due = Clock::now() + kDelay;
        qb::io::async::callback([this] { fired(); }, kDelay);
    }
    void
    fired() {
        const auto late = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - _due).count();
        const auto best = g_best_lateness_ns.load(std::memory_order_acquire);
        if (best < 0 || late < best)
            g_best_lateness_ns.store(late, std::memory_order_release);
        if (++_round >= kRounds) {
            kill();
            return;
        }
        arm();
    }

public:
    qb::io::async::task<bool>
    onInit() override {
        g_backend.store(qb::io::async::listener::current.backend(), std::memory_order_release);
        arm();
        co_return true;
    }
};

std::chrono::nanoseconds
push_to_delivery() {
    return std::chrono::nanoseconds{g_seen_at_ns.load(std::memory_order_acquire) - g_pushed_at_ns.load(std::memory_order_acquire)};
}

void
expect_pushed_and_seen() {
    ASSERT_NE(g_pushed_at_ns.load(std::memory_order_acquire), 0) << "the pusher never pushed";
    ASSERT_NE(g_seen_at_ns.load(std::memory_order_acquire), 0) << "the ping was never delivered";
}

TEST(CoreParkWake, SocketReadinessWakesACoreParkedInItsLoop) {
    // The echo core sleeps in its loop with a 500 ms cap. Each request lands well after the floor
    // — 20 ms of silence against a floor of zero — and must be answered at poll latency, not at the
    // cap: the bound is a tenth of the latency, so a single round that waited for the timeout
    // fails on its own. Across the rounds the process (echo core + this thread, which sleeps and
    // blocks in `read`) must have charged well under the wall time, or the core polled.
    reset_atoms();
    constexpr auto kLatency = 500ms;
    constexpr auto kGap     = 20ms;
    constexpr int  kRounds  = 20;
    qb::Main       main;
    main.core(0).setLatency(kLatency).setIdleSpin(0us);
    main.addActor<EchoActor>(0);
    main.start(true);
    while (g_port.load(std::memory_order_acquire) == 0 && !main.hasError())
        std::this_thread::yield();
    ASSERT_FALSE(main.hasError());

    tcp::socket sock;
    ASSERT_EQ(sock.connect_v4("127.0.0.1", g_port.load(std::memory_order_acquire)), 0);
    sock.set_nonblocking(false);
    const std::string req = "ping\n";
    char              buf[16];

    auto round_trip = [&]() -> std::chrono::nanoseconds {
        const auto t0 = Clock::now();
        if (sock.write(req.data(), req.size()) != static_cast<int>(req.size()))
            return -1ns;
        std::size_t got = 0;
        while (got < req.size()) {
            const int n = sock.read(buf + got, sizeof(buf) - got);
            if (n <= 0)
                return -1ns;
            got += static_cast<std::size_t>(n);
        }
        return Clock::now() - t0;
    };

    ASSERT_GT(round_trip(), 0ns) << "the warm-up round failed";
    const auto               cpu_before  = process_cpu_time();
    const auto               wall_before = Clock::now();
    std::chrono::nanoseconds worst       = 0ns;
    for (int i = 0; i < kRounds; ++i) {
        std::this_thread::sleep_for(kGap); // the core parks behind this
        const auto rtt = round_trip();
        ASSERT_GT(rtt, 0ns) << "round " << i << " failed";
        worst = std::max(worst, rtt);
        EXPECT_LT(rtt, kLatency / 10) << "round " << i
                                      << " waited for the park timeout: " << std::chrono::duration_cast<std::chrono::microseconds>(rtt).count()
                                      << " us";
    }
    const auto wall = Clock::now() - wall_before;
    const auto cpu  = process_cpu_time() - cpu_before;
    EXPECT_LT(cpu, wall / 2) << "the echo core polled between requests instead of parking: "
                             << std::chrono::duration_cast<std::chrono::microseconds>(cpu).count() << " us of CPU across "
                             << std::chrono::duration_cast<std::chrono::microseconds>(wall).count() << " us; worst round "
                             << std::chrono::duration_cast<std::chrono::microseconds>(worst).count() << " us";

    sock.disconnect();
    qb::Main::stop();
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(CoreParkWake, ASubMillisecondTimerFiresUnderAMillisecondOnACoreParkedInItsLoop) {
    // A core with nothing to do but a 200 µs callback parks in its loop with that timer bounding
    // the wait. `epoll_wait` takes whole milliseconds and libev rounds UP, so until qev asked the
    // kernel in nanoseconds (`epoll_pwait2`, Linux >= 5.11 -- Huly QB-196) the timer fired a full
    // millisecond late on a parked core: 1010 µs at p50, measured on WSL2 g++-14 against 0.2 µs on
    // a spinning one. The BEST of twenty rounds is asserted: the millisecond path cannot fire
    // before 1 ms, so one round under 800 µs is the nanosecond path and nothing else, and a loaded
    // host only ever moves a round upward. A SKIP, never a FAIL, where the libc or the kernel has
    // no `epoll_pwait2` and under any other backend: io_uring's wait keeps libev's millisecond
    // minimum, Windows' wepoll takes milliseconds and the kernel adds its own coalescing on top.
#if defined(__linux__) && EV_USE_EPOLL_PWAIT2
    {
        struct timespec    zero{0, 0};
        struct epoll_event e[1];
        const int          fd     = epoll_create1(0);
        const int          r      = fd >= 0 ? epoll_pwait2(fd, e, 1, &zero, nullptr) : -1;
        const bool         enosys = r < 0 && errno == ENOSYS;
        if (fd >= 0)
            close(fd);
        if (enosys)
            GTEST_SKIP() << "the kernel has no epoll_pwait2 (Linux < 5.11)";
    }
    reset_atoms();
    g_best_lateness_ns.store(-1, std::memory_order_release);
    g_backend.store(0, std::memory_order_release);
    qb::Main main;
    main.core(0).setLatency(500ms).setIdleSpin(0us);
    main.addActor<SubMsTimerActor>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    const auto backend = g_backend.load(std::memory_order_acquire);
    if (backend != EVBACKEND_EPOLL)
        GTEST_SKIP() << "the core's loop runs on " << qb::io::async::listener::backend_name(backend) << ", not epoll";
    const auto best = g_best_lateness_ns.load(std::memory_order_acquire);
    ASSERT_GE(best, 0) << "no round fired";
    EXPECT_LT(best, 800'000) << "every round of a 200 us timer on the parked core fired at the millisecond: best lateness "
                             << best / 1000 << " us";
#else
    GTEST_SKIP() << "epoll_pwait2 is not in this build (Linux with glibc >= 2.35 only)";
#endif
}

TEST(CoreParkWake, CrossCorePushWakesACoreParkedInItsLoop) {
    // Core 1 holds a 60 s timer, so it parks in its loop under a 2 s cap; core 0 pushes after
    // 150 ms. The producer finds `Park::Loop` published and sends the loop's `ev_async` — the
    // one path through which a mailbox event can end a loop park. Delivery must be far under the
    // cap: without the async send the ping would wait the full 2 s (the cap), and without the cap
    // the full 60 s (the timer).
    reset_atoms();
    constexpr auto kLatency = 2000ms; // ms, so `kLatency / 4` is 500 ms and not `2s / 4 == 0s`
    constexpr auto kDelay   = 150ms;
    qb::Main       main;
    main.core(0).setLatency(0ns);
    main.core(1).setLatency(kLatency).setIdleSpin(0us);
    const auto target = main.addActor<Target>(1, true);
    ASSERT_TRUE(target.is_valid());
    main.addActor<Pusher>(0, target, qb::duration{kDelay});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    expect_pushed_and_seen();
    const auto delay = push_to_delivery();
    EXPECT_LT(delay, kLatency / 4) << "the push waited for the park cap: delivered after "
                                   << std::chrono::duration_cast<std::chrono::milliseconds>(delay).count() << " ms";
}

TEST(CoreParkWake, CrossCorePushWakesACoreParkedOnItsMailbox) {
    // Same exchange, but core 1 owns no io watcher and parks on its condition variable — the
    // producer finds `Park::Cv` and signals it. The pre-3.2 path, kept, and pinned so the
    // three-state park cannot regress it.
    reset_atoms();
    constexpr auto kLatency = 2000ms; // ms, so `kLatency / 4` is 500 ms and not `2s / 4 == 0s`
    constexpr auto kDelay   = 150ms;
    qb::Main       main;
    main.core(0).setLatency(0ns);
    main.core(1).setLatency(kLatency).setIdleSpin(0us);
    const auto target = main.addActor<Target>(1, false);
    ASSERT_TRUE(target.is_valid());
    main.addActor<Pusher>(0, target, qb::duration{kDelay});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    expect_pushed_and_seen();
    const auto delay = push_to_delivery();
    EXPECT_LT(delay, kLatency / 4) << "the push waited for the park timeout: delivered after "
                                   << std::chrono::duration_cast<std::chrono::milliseconds>(delay).count() << " ms";
}

} // namespace core_park_wake_test
