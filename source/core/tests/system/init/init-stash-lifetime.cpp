/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-stash-lifetime.cpp
 * @brief CANONICAL owner of the activation-stash payload-lifetime contract (the leak-fix suite).
 *
 * A `push`'d event carrying a heap `std::string` that is stashed for an Activating actor must have
 * its payload DESTROYED — not leaked — on every termination path, and exactly-once delivered then
 * destroyed on the success path. The teeth: `qb::test::PayloadEvent` increments a global `live`
 * counter in every constructor and decrements it in its destructor, so `live == 0` after the engine
 * drains iff every constructed instance was destroyed. The qb event layer byte-relocates events
 * (no ctor/dtor on the relocation), so the only ctor is the placement-new at push and the only dtor
 * is the framework disposer; a non-zero `live` is a real leak. This suite runs under
 * ASAN_OPTIONS=detect_leaks=0, so LeakSanitizer would NOT catch the leak — the counter does — and
 * ASan still catches any double-free of the heap `data`.
 *
 * Five paths, each must zero the counter:
 *   1a. async `co_return false` after a `co_await`     → stash dropped + disposed (handler never runs);
 *   1b. killed during init (KillEvent passes the gate) → stash dropped + disposed (never replayed);
 *   1c. activation deadline expires                    → stash dropped + disposed;
 *   1d. stash overflow cap (> kActivationStashCap)     → the overflow forces fail + disposes all;
 *   1e. SUCCESS path                                   → replayed FIFO, each delivered then disposed.
 *
 * tier=system (real engine, real deferred-destroy + stash machinery). The success path also keeps
 * its FIFO-order and only-after-active teeth, each mirrored to a post-`join()` atomic.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::PayloadEvent;
using qb::test::ScopedDeadline;

namespace {

// A burst sender: pushes `_n` PayloadEvents at the target then self-kills.
class PayloadBurst : public qb::Actor {
    qb::ActorId _target;
    int         _n;

public:
    PayloadBurst(qb::ActorId t, int n)
        : _target(t)
        , _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 1; i <= _n; ++i)
            push<PayloadEvent>(_target, i);
        kill();
        co_return true;
    }
};

// ===========================================================================
// 1a. onInit co_returns false after a co_await: stash must be DISPOSED, not leaked.
// ===========================================================================
std::atomic<int> g_fail_handler_calls{0};

class FailsAfterAwait : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this); // registers the disposer for PayloadEvent
        co_await context().sleep(30ms);     // Activating while the burst piles up
        co_return false;                    // init fails → stash dropped (must be disposed)
    }
    void
    on(PayloadEvent &) {
        g_fail_handler_calls.fetch_add(1); // must NEVER run (actor failed init)
    }
};

TEST(InitStashLifetime, FailedAsyncInitDisposesStashedPayloads) {
    PayloadEvent::live.store(0);
    g_fail_handler_calls.store(0);
    {
        qb::Main   main;
        const auto victim = main.addActor<FailsAfterAwait>(0);
        main.addActor<PayloadBurst>(0, victim, 16);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_fail_handler_calls.load(), 0); // failed actor never handled a stashed event
    EXPECT_EQ(PayloadEvent::live.load(), 0L)   // every stashed payload was destroyed
        << "stashed events leaked when async init failed";
}

// ===========================================================================
// 1b. Killed during init: KillEvent passes the gate; stash dropped + disposed.
// ===========================================================================
std::atomic<int> g_killed_handler_calls{0};

class ParksThenKilled : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().until_cancelled(); // parks until killed; throws cancelled_error
        co_return true;                        // unreachable
    }
    void
    on(PayloadEvent &) {
        g_killed_handler_calls.fetch_add(1);
    }
};

class BurstThenKill : public qb::Actor {
    qb::ActorId _target;
    int         _n;

public:
    BurstThenKill(qb::ActorId t, int n)
        : _target(t)
        , _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 1; i <= _n; ++i)
            push<PayloadEvent>(_target, i); // stashed (target Activating)
        push<qb::KillEvent>(_target);       // must pass the gate and unwind the in-flight init
        kill();
        co_return true;
    }
};

TEST(InitStashLifetime, KillDuringInitPassesGateAndDisposesStash) {
    PayloadEvent::live.store(0);
    g_killed_handler_calls.store(0);
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksThenKilled>(0);
        main.addActor<BurstThenKill>(0, victim, 12);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_killed_handler_calls.load(), 0); // killed-during-init: stash never replayed
    EXPECT_EQ(PayloadEvent::live.load(), 0L)
        << "stashed payloads leaked when the actor was killed during init";
}

// ===========================================================================
// 1c. Activation deadline expires: stash dropped + disposed.
// ===========================================================================
class ParksForever : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().until_cancelled(); // never completes on its own
        co_return true;
    }
    void
    on(PayloadEvent &) {}
};

TEST(InitStashLifetime, ActivationDeadlineDisposesStash) {
    PayloadEvent::live.store(0);
    ScopedDeadline dl(80ull * 1000ull * 1000ull); // 80ms deadline
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksForever>(0);
        main.addActor<PayloadBurst>(0, victim, 10);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(PayloadEvent::live.load(), 0L)
        << "stashed payloads leaked when the activation deadline expired";
}

// ===========================================================================
// 1d. Stash overflow cap: the dropped overflow events are disposed too.
// ===========================================================================
TEST(InitStashLifetime, StashOverflowDisposesAllDroppedPayloads) {
    PayloadEvent::live.store(0);
    // Overflowing the cap itself forces the activation to fail (the actor parks until that
    // fail tears it down), so no ScopedDeadline is needed.
    constexpr int kBurst = 4200; // > kActivationStashCap (4096)
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksForever>(0);
        main.addActor<PayloadBurst>(0, victim, kBurst);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(PayloadEvent::live.load(), 0L)
        << "overflowed/stashed payloads leaked when the stash cap was exceeded";
}

// ===========================================================================
// 1e. Success path: stash replayed in FIFO order, each payload delivered then disposed.
// ===========================================================================
std::atomic<int>  g_ok_count{0};
std::atomic<bool> g_ok_order{true};
std::atomic<bool> g_ok_all_after_active{true};

class ActivatesAndConsumes : public qb::Actor {
    int _next = 1;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().sleep(30ms);
        co_return true; // success → stash replayed
    }
    void
    on(PayloadEvent &e) {
        if (!is_active())
            g_ok_all_after_active.store(false); // replay must happen only once active
        if (e.seq != _next++)
            g_ok_order.store(false);
        if (g_ok_count.fetch_add(1) + 1 == 8)
            kill();
    }
};

TEST(InitStashLifetime, SuccessReplaysFifoAndDisposesEachPayload) {
    PayloadEvent::live.store(0);
    g_ok_count.store(0);
    g_ok_order.store(true);
    g_ok_all_after_active.store(true);
    {
        qb::Main   main;
        const auto victim = main.addActor<ActivatesAndConsumes>(0);
        main.addActor<PayloadBurst>(0, victim, 8);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_ok_count.load(), 8);
    EXPECT_TRUE(g_ok_order.load());            // FIFO preserved across the activation boundary
    EXPECT_TRUE(g_ok_all_after_active.load()); // nothing delivered before activation
    EXPECT_EQ(PayloadEvent::live.load(), 0L);  // replayed events disposed exactly once
}

} // namespace
