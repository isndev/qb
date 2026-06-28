/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/concurrency/shutdown-saturation.cpp
 * @brief Engine shutdown must terminate when a peer core exits while a saturated mailbox is
 *        still being flushed (regression for the residual-drain livelock).
 *
 * Source cores flood a deliberately slow sink with non-trivial (heap-owning, default QoS-2)
 * events until its mailbox is permanently saturated, then the engine is stopped mid-flood. The
 * sink core leaves __workflow__ and stops draining its mailbox while the sources still hold
 * undeliverable events queued for it. The fixed residual drain must:
 *   - TERMINATE (the old unbounded `while (__flush_all__())` livelocked forever, hanging join());
 *   - drop nothing a still-live peer can accept (no premature drop — guarded by the per-core
 *     "stopped" flag); and
 *   - free every non-trivial payload exactly once (the dispose paths + the post-join mailbox
 *     sweep) — checked by ASan/LSan in the sanitized CI build, and a net live-payload counter
 *     here for the no-sanitizer build.
 *
 * An in-test watchdog converts a regression (livelock) into a clean test FAILURE instead of
 * relying solely on the ctest timeout.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

// Net count of live HeavyEvent payloads: ctor/copy ++ , dtor -- . Must return to 0 after the
// engine is destroyed (every event delivered-and-disposed, dropped-and-disposed, or swept).
std::atomic<std::int64_t> g_live_payloads{0};

struct Tracked {
    std::string s;
    Tracked() : s(48, 'x') { g_live_payloads.fetch_add(1, std::memory_order_relaxed); }
    Tracked(const Tracked &o) : s(o.s) { g_live_payloads.fetch_add(1, std::memory_order_relaxed); }
    Tracked(Tracked &&o) noexcept : s(std::move(o.s)) { g_live_payloads.fetch_add(1, std::memory_order_relaxed); }
    Tracked &operator=(const Tracked &) = default;
    Tracked &operator=(Tracked &&)      = default;
    ~Tracked() { g_live_payloads.fetch_sub(1, std::memory_order_relaxed); }
};

// Default QoS = 2, NON-trivially-destructible (owns a heap std::string via Tracked).
struct HeavyEvent : public qb::Event {
    Tracked       payload;
    std::uint32_t seq;
    explicit HeavyEvent(std::uint32_t s) : seq(s) {}
};

std::atomic<std::uint64_t> g_received{0};
std::atomic<std::uint32_t> g_sink_id{0};

class SlowSinkActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<HeavyEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        g_sink_id.store(static_cast<std::uint32_t>(id()), std::memory_order_release);
        co_return true;
    }
    void
    on(HeavyEvent const &e) {
        g_received.fetch_add(e.payload.s.empty() ? 0u : 1u, std::memory_order_relaxed);
        // Slow drain keeps the mailbox saturated. A volatile sink with plain assignment (not a
        // ++ on a volatile, which C++20 deprecates) stops the loop being optimised away.
        volatile int sink = 0;
        for (int i = 0; i < 256; ++i)
            sink = sink + i;
        (void) sink;
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

class FloodSourceActor
    : public qb::Actor
    , public qb::ICallback {
    std::uint32_t _seq = 0;

public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::KillEvent const &) {
        unregisterCallback();
        kill();
    }
    void
    on(qb::LoopEvent const &) final {
        const std::uint32_t sink = g_sink_id.load(std::memory_order_acquire);
        if (!sink)
            return;
        const qb::ActorId dest(sink);
        for (int i = 0; i < 3000; ++i)
            push<HeavyEvent>(dest, _seq++);
    }
};

} // namespace

TEST(ShutdownSaturation, StopMidFloodTerminatesAndLeaksNothing) {
    if (std::thread::hardware_concurrency() < 3) {
        GTEST_SKIP() << "needs >= 3 hardware cores (1 sink + >= 2 sources)";
    }

    constexpr int kIterations = 5;
    const qb::CoreId nsrc      = 2;

    for (int iter = 0; iter < kIterations; ++iter) {
        g_sink_id.store(0, std::memory_order_relaxed);

        std::atomic<bool> finished{false};
        // Watchdog: a regression livelocks join() forever; fail loudly instead of hanging.
        std::thread watchdog([&finished] {
            for (int t = 0; t < 200; ++t) { // up to 20 s
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (finished.load(std::memory_order_acquire))
                    return;
            }
            ADD_FAILURE() << "engine shutdown livelocked (residual-drain regression)";
            std::abort();
        });

        {
            qb::Main main;
            main.core(0).addActor<SlowSinkActor>();
            for (qb::CoreId c = 1; c <= nsrc; ++c)
                main.core(c).addActor<FloodSourceActor>();
            main.start(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40)); // let the mailbox saturate
            main.stop();
            main.join(); // must return — the residual drain terminates against a stopped peer
        } // ~Main: a second join() sweeps any residual mailbox events

        finished.store(true, std::memory_order_release);
        watchdog.join();
    }

    // Every HeavyEvent payload constructed must have been destroyed exactly once.
    EXPECT_EQ(g_live_payloads.load(std::memory_order_relaxed), 0)
        << "non-trivial QoS-2 payloads leaked at shutdown";
    EXPECT_GT(g_received.load(std::memory_order_relaxed), 0u) << "the sink received no events";
}
