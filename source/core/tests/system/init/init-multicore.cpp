/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-multicore.cpp
 * @brief Cross-core async-init: the activation gate and the kill-during-init path across cores.
 *
 * The activation gate lives on the destination core, so an event sent from a *different* core to a
 * still-Activating actor must be stashed remotely (on the destination core) and replayed FIFO once
 * that actor activates; a cross-core unicast `KillEvent` must still pass the gate and unwind an
 * in-flight `onInit`. This file proves both across a real two-core engine:
 *
 *   - CrossCoreUnicastToActivatingIsStashed — a burst sent core-0 → core-1 (Activating) is stashed
 *     on core 1 and replayed in order strictly after the destination's init completes;
 *   - CrossCoreKillOfActivatingActor       — a kill sent core-0 → core-1 cancels the long init
 *     (it never completes) and the actor is destroyed cleanly (deferred destroy).
 *
 * Both cases require >= 2 cores; on a single-core runner they GTEST_SKIP with a
 * "requires-multicore: ..." message rather than hard-failing. Every in-actor effect is mirrored to
 * a post-`join()` atomic so a never-scheduled actor cannot pass the test vacuously.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::Tick;

namespace {

// ===========================================================================
// Cross-core unicast burst to an Activating actor → stashed remotely, replayed FIFO.
// ===========================================================================
std::atomic<int>  g_xc_count{0};
std::atomic<bool> g_xc_inited{false};
std::atomic<bool> g_xc_after{true};
std::atomic<bool> g_xc_order{true};

class XCoreSlow : public qb::Actor {
    int _expected_next = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(40ms);
        g_xc_inited.store(true);
        co_return true;
    }
    void
    on(Tick &e) {
        if (!g_xc_inited.load())
            g_xc_after.store(false); // a stashed event must never land before activation
        if (e.n != _expected_next++)
            g_xc_order.store(false); // FIFO order must survive the cross-core stash
        if (g_xc_count.fetch_add(1) + 1 == 4)
            kill();
    }
};

class XCoreSender : public qb::Actor {
    qb::ActorId _t;

public:
    explicit XCoreSender(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 4; ++i)
            push<Tick>(_t, i); // cross-core unicast to an Activating actor → stashed remotely
        kill();
        co_return true;
    }
};

TEST(InitMulticore, CrossCoreUnicastToActivatingIsStashed) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise the cross-core activation stash";
    g_xc_count.store(0);
    g_xc_inited.store(false);
    g_xc_after.store(true);
    g_xc_order.store(true);
    qb::Main   main;
    const auto slow = main.addActor<XCoreSlow>(1); // Activating on core 1
    main.addActor<XCoreSender>(0, slow);           // sender on core 0
    main.start(false);
    main.join();
    EXPECT_EQ(g_xc_count.load(), 4); // all four replayed
    EXPECT_TRUE(g_xc_after.load());  // all delivered strictly after activation
    EXPECT_TRUE(g_xc_order.load());  // in FIFO order across the cross-core stash
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// Cross-core kill of an Activating actor → cancels the init, destroys cleanly.
// ===========================================================================
std::atomic<bool> g_xck_started{false};
std::atomic<bool> g_xck_done{false};
std::atomic<bool> g_xck_destroyed{false};

class XCoreLong : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_xck_started.store(true);
        co_await context().sleep(500ms); // cancelled by the cross-core kill long before this elapses
        g_xck_done.store(true);          // must NOT happen
        co_return true;
    }
    ~XCoreLong() override {
        g_xck_destroyed.store(true);
    }
};

class XCoreKiller : public qb::Actor {
    qb::ActorId _t;

public:
    explicit XCoreKiller(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // let XCoreLong enter its long init first
        push<qb::KillEvent>(_t);        // cross-core unicast kill of an Activating actor
        kill();
        co_return true;
    }
};

TEST(InitMulticore, CrossCoreKillOfActivatingActor) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise a cross-core kill-during-init";
    g_xck_started.store(false);
    g_xck_done.store(false);
    g_xck_destroyed.store(false);
    qb::Main   main;
    const auto t = main.addActor<XCoreLong>(1);
    main.addActor<XCoreKiller>(0, t);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_xck_started.load());   // the long init started on core 1
    EXPECT_FALSE(g_xck_done.load());     // ...but was cancelled by the cross-core kill
    EXPECT_TRUE(g_xck_destroyed.load()); // actor destroyed cleanly (deferred destroy)
    EXPECT_FALSE(main.hasError());
}

} // namespace
