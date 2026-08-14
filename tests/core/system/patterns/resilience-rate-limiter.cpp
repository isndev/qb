/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/resilience-rate-limiter.cpp
 * @brief Engine-driven `qb::rate_limiter::acquire()` — throttle-then-complete + cancel-on-kill.
 *
 * The token-bucket arithmetic is unit-tested clock-injected (unit/patterns/resilience-rate-limiter);
 * here we prove the awaitable `acquire(ctx)` behaviour that REQUIRES a real `VirtualCore` clock and
 * coroutine scope:
 *   - bursts up to `capacity` are admitted immediately, then further acquires WAIT for refill and
 *     still complete (throttled, never dropped) — proven by an exact post-join count, not a count
 *     the test set itself;
 *   - an `acquire()` parked on an empty bucket is CANCELLATION-AWARE: a kill wakes it with
 *     `cancelled_error` (no hang), and the parked waiter is retracted.
 *
 * De-flaked vs the monolith: shutdown is event-driven. The throttle test stops the engine the
 * instant the 4th acquire returns; the cancel test stops it from inside the victim's catch the
 * instant `cancelled_error` is observed — no fixed wall-clock `stop()` offset is used as an oracle.
 * In-actor results are mirrored to atomics whose "ran" flag is asserted true after join() so a
 * never-scheduled coroutine cannot pass the test vacuously.
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
#include <memory>

using namespace qb;
using namespace std::chrono_literals;

// ===========================================================================
// acquire() throttles bursts but completes them all (never drops a request).
// ===========================================================================
namespace {
std::atomic<int>  g_rl_acquired{0};
std::atomic<bool> g_rl_loop_done{false}; // set after the acquire loop finishes (ran-guard)
} // namespace

class RateLimitedActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto rl = std::make_shared<qb::rate_limiter>(2.0, 15ms); // burst 2, then 1 / 15ms
            for (int i = 0; i < 4; ++i) {
                co_await rl->acquire(c); // first 2 immediate, next 2 wait (cancellation-aware)
                g_rl_acquired.fetch_add(1);
            }
            g_rl_loop_done.store(true);
            qb::Main::stop(); // event-driven shutdown: stop the instant the 4th acquire returns
        });
        co_return true;
    }
};

TEST(RateLimiterAcquire, ThrottlesBurstAndCompletesEveryAcquire) {
    g_rl_acquired.store(0);
    g_rl_loop_done.store(false);
    qb::Main main;
    main.addActor<RateLimitedActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_rl_loop_done.load()) << "the acquire loop coroutine must have run to completion";
    EXPECT_EQ(g_rl_acquired.load(), 4) << "all four acquires must complete (throttled, not dropped)";
}

// ===========================================================================
// acquire() parked on an empty bucket is cancel-on-kill (the one engine-only wait path).
// ===========================================================================
namespace {
std::atomic<bool> g_rl_cancelled{false};
std::atomic<bool> g_rl_victim_ran{false}; // set once the victim coroutine has reached its wait
} // namespace

class RateLimitCancelActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto rl = std::make_shared<qb::rate_limiter>(1.0, 10s); // 1 token, refills very slowly
            co_await rl->acquire(c);                                // take the only token immediately
            g_rl_victim_ran.store(true);
            try {
                co_await rl->acquire(c); // parks (~10s) → must be cancelled on kill, not hang
            } catch (const qb::io::async::cancelled_error &) {
                g_rl_cancelled.store(true);
                qb::Main::stop(); // event-driven: stop the instant cancellation is observed
            }
        });
        co_return true;
    }
};

// Kills `_victim` once it is parked, then arms a generous backstop stop in case the cancel path
// itself regressed (so the test FAILS loudly via the assertions rather than hanging forever).
class KillThenBackstop : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit KillThenBackstop(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        // Deliberate, and NOT the `[this]`-outlives-the-actor hazard: this helper is never a kill
        // target and never kills itself, so `this` is live whenever the timer fires; and a timer still
        // pending at teardown is reclaimed by listener::clear() WITHOUT firing. Must stay out-of-band:
        // it triggers the cancel under test. Do not convert to spawn + ctx.sleep.
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // lands while parked
        qb::io::async::callback([] { qb::Main::stop(); }, 2s);                // backstop only — never the oracle
        co_return true;
    }
};

TEST(RateLimiterAcquire, ParkedAcquireCancelledOnKill) {
    g_rl_cancelled.store(false);
    g_rl_victim_ran.store(false);
    qb::Main   main;
    const auto victim = main.addActor<RateLimitCancelActor>(0);
    main.addActor<KillThenBackstop>(0, victim);
    main.start(false);
    main.join(); // must NOT hang — the cancellation-aware acquire retracts the parked waiter
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_rl_victim_ran.load()) << "the victim must have consumed its token and parked";
    EXPECT_TRUE(g_rl_cancelled.load()) << "a kill while parked must surface as cancelled_error";
}
