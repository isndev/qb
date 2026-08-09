/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-gate-edges.cpp
 * @brief Edges of the activation dispatch gate: single- and multi-sender FIFO stash, broadcast pass-through.
 *
 * The activation gate (`VirtualCore::__receive_events__`) defers inbound *unicast business* events
 * for a still-Activating actor into a per-actor FIFO stash, replaying them in order once it activates,
 * while letting broadcasts pass straight through. This file pins the gate's edge behaviors:
 *
 *   - StashedEventsReplayedInOrder      — a single sender's burst stashes while the target is
 *                                         Activating and replays FIFO, strictly after activation
 *                                         (the base stash-replay edge);
 *   - MultipleSendersAllStashedAndReplayed — three independent senders' bursts (9 events) all stash
 *                                         and replay after activation, none lost;
 *   - CustomBroadcastPassesGateWhileActivating — a NON-Kill custom broadcast is NOT stashed: it
 *                                         passes the gate and is delivered to the Activating actor at once.
 *
 * tier=system. Counts are EXACT and the "only after activation" invariant is mirrored to a
 * post-`join()` atomic, so a stashed event that leaked through early — or a broadcast wrongly stashed —
 * fails loudly.
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
using qb::test::Tick;

namespace {

// ===========================================================================
// 1. Single-sender burst → stashed while Activating, replayed FIFO after activation.
// ===========================================================================
std::atomic<bool> g_so_inited{false};
std::atomic<int>  g_so_count{0};
std::atomic<bool> g_so_order{true};
std::atomic<bool> g_so_all_after{true};

class SlowConsumer : public qb::Actor {
    int _expected_next = 1;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(40ms); // long enough for the burst to pile up while Activating
        g_so_inited.store(true);
        co_return true;
    }
    void
    on(Tick &e) {
        if (!g_so_inited.load())
            g_so_all_after.store(false); // a stashed event must never land before activation
        if (e.n != _expected_next)
            g_so_order.store(false); // FIFO order must be preserved
        ++_expected_next;
        if (g_so_count.fetch_add(1) + 1 == 5)
            kill();
    }
};

class BurstSender : public qb::Actor {
    qb::ActorId _target;

public:
    explicit BurstSender(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        // Synchronous init: fire the whole burst while SlowConsumer is still Activating.
        for (int i = 1; i <= 5; ++i)
            push<Tick>(_target, i);
        kill();
        co_return true;
    }
};

TEST(InitGateEdges, StashedEventsReplayedInOrderAfterActivation) {
    g_so_inited.store(false);
    g_so_count.store(0);
    g_so_order.store(true);
    g_so_all_after.store(true);

    qb::Main   main;
    const auto slow = main.addActor<SlowConsumer>(0);
    main.addActor<BurstSender>(0, slow);
    main.start(false);
    main.join();

    EXPECT_EQ(g_so_count.load(), 5);    // all five replayed
    EXPECT_TRUE(g_so_order.load());     // in FIFO order
    EXPECT_TRUE(g_so_all_after.load()); // never before onInit completed
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 2. Multiple senders → all stashed and replayed (3 senders x 3 events = 9).
// ===========================================================================
std::atomic<int>  g_ms_count{0};
std::atomic<bool> g_ms_after{true};
std::atomic<bool> g_ms_inited{false};

class MultiStashVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(40ms);
        g_ms_inited.store(true);
        co_return true;
    }
    void
    on(Tick &) {
        if (!g_ms_inited.load())
            g_ms_after.store(false);
        if (g_ms_count.fetch_add(1) + 1 == 9) // 3 senders x 3 events
            kill();
    }
};

class MiniSender : public qb::Actor {
    qb::ActorId _t;

public:
    explicit MiniSender(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 3; ++i)
            push<Tick>(_t, i);
        kill();
        co_return true;
    }
};

TEST(InitGateEdges, MultipleSendersAllStashedAndReplayed) {
    g_ms_count.store(0);
    g_ms_after.store(true);
    g_ms_inited.store(false);
    qb::Main   main;
    const auto v = main.addActor<MultiStashVictim>(0);
    main.addActor<MiniSender>(0, v);
    main.addActor<MiniSender>(0, v);
    main.addActor<MiniSender>(0, v);
    main.start(false);
    main.join();
    EXPECT_EQ(g_ms_count.load(), 9); // all 9 stashed across 3 senders, replayed after activation
    EXPECT_TRUE(g_ms_after.load());
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 3. A custom (non-Kill) broadcast is NOT stashed — it passes the gate at once.
// ===========================================================================
struct Ping2 : public qb::Event {};
std::atomic<bool> g_cb_received{false};
std::atomic<bool> g_cb_while_activating{false};
// Set by the victim the instant it enters its Activating window. The sender waits on THIS
// rather than guessing the moment with a fixed sleep -- see the note on Bcaster below.
std::atomic<bool> g_victim_activating{false};

class BcastVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping2>(*this);
        g_victim_activating.store(true);
        // 300ms, not 40ms. What this case asserts is that the broadcast is HANDLED while the
        // actor is still Activating, so the window has to outlast the scheduling latency of
        // one broadcast -- and that latency is not bounded by anything the test controls. At
        // 40ms against a sender that woke at 10ms the margin was 30ms, which the preset's own
        // `jobs: 4` closed: this case failed under parallel ctest on Windows while passing 8/8
        // in isolation. A margin is the only thing that makes a wall-clock assertion honest,
        // and 300ms costs one third of a second once.
        co_await context().sleep(300ms); // still Activating when the broadcast arrives
        co_return true;
    }
    void
    on(Ping2 &) {
        // A broadcast is NOT stashed — it passes the gate while we are still Activating.
        if (!is_active())
            g_cb_while_activating.store(true);
        g_cb_received.store(true);
        kill();
    }
};

class Bcaster : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Wait for the victim to ACTUALLY be in its window instead of guessing that a fixed
        // 10ms sleep lands inside it. The guess was the second half of the same flakiness: it
        // spends part of the victim's margin before the broadcast is even sent, and on Windows
        // a 10ms timer is quantised up to the ~15.6ms system tick, so what the test called
        // "10ms" was never 10ms. Polling costs nothing and fires as early as it possibly can.
        while (!g_victim_activating.load())
            co_await context().sleep(1ms);
        broadcast<Ping2>();
        kill();
        co_return true;
    }
};

TEST(InitGateEdges, CustomBroadcastPassesGateWhileActivating) {
    g_cb_received.store(false);
    g_cb_while_activating.store(false);
    g_victim_activating.store(false);
    qb::Main main;
    main.addActor<BcastVictim>(0);
    main.addActor<Bcaster>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_cb_received.load()) << "custom broadcast must reach the Activating actor (not stashed)";
    EXPECT_TRUE(g_cb_while_activating.load()) << "the broadcast must be delivered DURING the Activating phase, proving it bypassed the stash";
    EXPECT_FALSE(main.hasError());
}

} // namespace
