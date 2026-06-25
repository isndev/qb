/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/discovery-ping-require.cpp
 * @brief Coroutine actor discovery & liveness: `qb::ping` (targeted) + `qb::require<T>` (typed).
 *
 * The modern awaitable replacement for the legacy `require<T>()` + `on(RequireEvent&)` + `is<T>()`
 * dance. Built on the kernel `PingEvent`/`RequireEvent` (which carry an echoed correlation id), with
 * replies routed automatically by `Actor`'s default `on(RequireEvent&)` — no handler boilerplate.
 * Proven here against the running engine:
 *   - PING (alive): a known live target replies → `ping` returns true;
 *   - PING (dead): a default/invalid target never replies → `ping` returns false after its window;
 *   - REQUIRE (all): three live `DiscWorker`s are all discovered within the window (exact count 3);
 *   - REQUIRE (none-of-type): with a live actor of a DIFFERENT type present, `require<GhostWorker>`
 *     returns EMPTY when no actor of that type exists (it must not match the unrelated live actor);
 *   - PING cancel: killed while parked on a never-answering ping → `cancelled_error` (no hang).
 *
 * Cleanups vs the monolith: the `qb::io::async::listener::current.clear()` cross-test isolation
 * smell is REMOVED — each test owns its own `qb::Main`, so there is no leftover timer to scrub.
 * Results are mirrored to atomics behind a "ran" flag asserted after join(); shutdown is event-
 * driven from the probe coroutine the instant discovery resolves (the cancel test arms only a
 * generous backstop stop).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int>  g_disc_alive{-1};   // ping known target
std::atomic<int>  g_disc_dead{-1};    // ping invalid target
std::atomic<int>  g_disc_count{-1};   // require<DiscWorker> count
std::atomic<int>  g_disc_empty{-1};   // require<GhostWorker> count (expected 0)
std::atomic<bool> g_disc_cancelled{false};
std::atomic<bool> g_disc_ran{false};  // the probe coroutine reached its terminal state
} // namespace

// Plain actor: the kernel auto-registers PingEvent, so it answers discovery/liveness out of the box.
class DiscWorker : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

// A type that is never instantiated — require<GhostWorker> must find none of them.
class GhostWorker : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

class DiscProbe : public qb::Actor {
    qb::ActorId _target;

public:
    explicit DiscProbe(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        // No registerEvent<RequireEvent> / on(RequireEvent) — Actor routes discovery replies by default.
        auto target = _target;
        spawn([target](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            g_disc_alive.store(co_await qb::ping(c, target, 150ms) ? 1 : 0);       // alive
            g_disc_dead.store(co_await qb::ping(c, qb::ActorId{}, 100ms) ? 1 : 0); // invalid → timeout
            auto found = co_await qb::require<DiscWorker>(c, 120ms);               // discover all
            g_disc_count.store(static_cast<int>(found.size()));
            g_disc_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant discovery resolves
        });
        co_return true;
    }
};

TEST(Discovery, PingAndRequire) {
    g_disc_alive.store(-1);
    g_disc_dead.store(-1);
    g_disc_count.store(-1);
    g_disc_ran.store(false);
    qb::Main   main;
    const auto w0 = main.addActor<DiscWorker>(0);
    main.addActor<DiscWorker>(0);
    main.addActor<DiscWorker>(0);
    main.addActor<DiscProbe>(0, w0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_disc_ran.load()) << "the discovery probe coroutine must have run";
    EXPECT_EQ(g_disc_alive.load(), 1) << "a known live worker must reply to ping";
    EXPECT_EQ(g_disc_dead.load(), 0) << "an invalid id must time out (no reply)";
    EXPECT_EQ(g_disc_count.load(), 3) << "all three DiscWorkers discovered within the window";
}

// require<T> with a live actor of a DIFFERENT type present must return empty (no false match).
class RequireEmptyProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto found = co_await qb::require<GhostWorker>(c, 120ms);
            g_disc_empty.store(static_cast<int>(found.size()));
            g_disc_ran.store(true);
            qb::Main::stop();
        });
        co_return true;
    }
};

TEST(Discovery, RequireEmptyWhenNoneOfType) {
    g_disc_empty.store(-1);
    g_disc_ran.store(false);
    qb::Main main;
    main.addActor<DiscWorker>(0); // a live actor of a DIFFERENT type — must not match
    main.addActor<RequireEmptyProbe>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_disc_ran.load());
    EXPECT_EQ(g_disc_empty.load(), 0) << "window elapsed with no GhostWorker → empty, no false match";
}

// Parks a ping on a never-answering target, is killed, and must surface cancelled_error.
class DiscProbeCancel : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                co_await qb::ping(c, qb::ActorId{}, 5s); // never answers; long wait
            } catch (const qb::io::async::cancelled_error &) {
                g_disc_cancelled.store(true);
                qb::Main::stop(); // event-driven: stop the instant the parked ping is cancelled
            }
            g_disc_ran.store(true);
        });
        co_return true;
    }
};

class DiscKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit DiscKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill while parked
        qb::io::async::callback([] { qb::Main::stop(); }, 2s);                 // backstop only
        co_return true;
    }
};

TEST(Discovery, PingCancelledOnKill) {
    g_disc_cancelled.store(false);
    g_disc_ran.store(false);
    qb::Main   main;
    const auto victim = main.addActor<DiscProbeCancel>(0);
    main.addActor<DiscKiller>(0, victim);
    main.start(false);
    main.join(); // must not hang — kill wakes the parked ping, which throws cancelled
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_disc_cancelled.load()) << "a kill while parked on ping must surface as cancelled";
}
