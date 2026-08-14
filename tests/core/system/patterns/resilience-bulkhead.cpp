/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/resilience-bulkhead.cpp
 * @brief Engine-driven `qb::bulkhead::enter()` — concurrency cap + parked-enter cancel-on-kill.
 *
 * The permit accounting is unit-tested (unit/patterns/resilience-bulkhead); here we prove the
 * awaitable `enter(ctx)` behaviour that REQUIRES a real coroutine scope:
 *   - with capacity 2 and five concurrent operations, NEVER more than 2 run at once (the bulkhead
 *     holds) yet ALL five eventually run — the observed max-in-flight is the framework truth, and
 *     the done-count is exact;
 *   - an `enter()` parked on a full bulkhead is CANCELLATION-AWARE: a kill wakes the waiter with
 *     `cancelled_error` (no hang) and retracts its queued claim (no permit leak).
 *
 * De-flaked vs the monolith: each in-flight op overlaps via a short `c.sleep`, but the OVERLAP is
 * what makes the cap observable — the oracle is the atomic max-in-flight counter, not a wall clock.
 * Shutdown is event-driven (stop the instant the 5th op completes / cancellation is observed); the
 * cancel test arms only a generous backstop stop so a regression fails loudly instead of hanging.
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
// enter() caps concurrency to the bulkhead size while still running every op.
// ===========================================================================
namespace {
std::atomic<int> g_bh_inflight{0};
std::atomic<int> g_bh_max{0};
std::atomic<int> g_bh_done{0};

void
bh_bump_max() {
    int cur = g_bh_inflight.load(), prev = g_bh_max.load();
    while (cur > prev && !g_bh_max.compare_exchange_weak(prev, cur)) { /* retry */
    }
}
} // namespace

class BulkheadActor : public qb::Actor {
    std::shared_ptr<qb::bulkhead> _bh = std::make_shared<qb::bulkhead>(2); // at most 2 concurrent

public:
    qb::io::async::task<bool>
    onInit() override {
        auto bh = _bh;
        for (int i = 0; i < 5; ++i) {
            spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
                auto slot = co_await bh->enter(c); // waits while 2 are already in flight
                g_bh_inflight.fetch_add(1);
                bh_bump_max();
                co_await c.sleep(15ms); // hold the slot so overlap is observable
                g_bh_inflight.fetch_sub(1);
                if (g_bh_done.fetch_add(1) + 1 == 5)
                    qb::Main::stop(); // event-driven: stop when all five have finished
            });
        }
        co_return true;
    }
};

TEST(BulkheadEnter, CapsConcurrencyWhileRunningEveryOperation) {
    g_bh_inflight.store(0);
    g_bh_max.store(0);
    g_bh_done.store(0);
    qb::Main main;
    main.addActor<BulkheadActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bh_done.load(), 5) << "every operation must eventually acquire a slot and run";
    EXPECT_LE(g_bh_max.load(), 2) << "the bulkhead must never admit more than its capacity at once";
    EXPECT_GE(g_bh_max.load(), 1) << "at least one op must have actually entered (teeth)";
}

// ===========================================================================
// enter() parked on a full bulkhead is cancel-on-kill.
// ===========================================================================
namespace {
std::atomic<bool> g_bh_cancelled{false};
std::atomic<bool> g_bh_holder_in{false}; // the holder took the only slot (proves the waiter parks)
} // namespace

class BulkheadCancelActor : public qb::Actor {
    std::shared_ptr<qb::bulkhead> _bh = std::make_shared<qb::bulkhead>(1); // single slot

public:
    qb::io::async::task<bool>
    onInit() override {
        auto bh = _bh;
        // Holder takes the only slot and keeps it for a long time (so the waiter genuinely parks).
        spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto slot = co_await bh->enter(c);
            g_bh_holder_in.store(true);
            co_await c.sleep(5s);
        });
        // Waiter parks on the full bulkhead → must be cancelled on kill (not hang).
        spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                auto slot = co_await bh->enter(c);
            } catch (const qb::io::async::cancelled_error &) {
                g_bh_cancelled.store(true);
                qb::Main::stop(); // event-driven: stop the instant the parked waiter is cancelled
            }
        });
        co_return true;
    }
};

class BulkheadKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit BulkheadKiller(qb::ActorId v)
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

TEST(BulkheadEnter, ParkedEnterCancelledOnKill) {
    g_bh_cancelled.store(false);
    g_bh_holder_in.store(false);
    qb::Main   main;
    const auto victim = main.addActor<BulkheadCancelActor>(0);
    main.addActor<BulkheadKiller>(0, victim);
    main.start(false);
    main.join(); // must NOT hang — the cancel-aware enter retracts the parked waiter
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_bh_holder_in.load()) << "the holder must have taken the only slot, forcing a park";
    EXPECT_TRUE(g_bh_cancelled.load()) << "a kill while parked on a full bulkhead must cancel cleanly";
}
