/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-handle.cpp
 * @brief `qb::ActorHandle` phase-awareness across the async-init boundary.
 *
 * `addRefActor<Child>()` returns a phase-aware handle whose `id()` is valid immediately (even while
 * the child is still Activating) but whose `ready()` / `get()` resolve only once the child is active.
 * `co_await handle.ready_async(ctx, timeout)` blocks (cancellation-aware) until the child activates
 * or the timeout elapses. This file proves the full handle contract around async init:
 *
 *   - ReadyAsyncAndIdStash       — an async child is valid-but-not-ready while Activating; an event
 *                                  pushed to `id()` meanwhile is stashed and replayed after it
 *                                  activates; `ready_async` resolves true and a deref-when-ready is safe;
 *   - SyncChildReadyImmediately  — a sync-init child is `ready()` the instant `addRefActor` returns;
 *   - BroadcastShutdownCancels   — a broadcast `KillEvent` reaches a still-Activating victim, cancels
 *                                  its init mid-flight, and destroys it cleanly;
 *   - ReadyAsyncTimesOut         — `ready_async` returns false when the child never activates in time;
 *   - ParentKilledMidReadyAsync  — a parent killed while awaiting `ready_async` unwinds cleanly.
 *
 * tier=system. Every in-actor observation (valid/ready, stash-served, timeout result) is mirrored to
 * a post-`join()` atomic so a never-scheduled parent cannot pass the test vacuously.
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
// 1. Async child: valid-but-not-ready while Activating, id()-stash, ready_async.
// ===========================================================================
std::atomic<bool> g_h_served{false};
std::atomic<bool> g_h_ready_seen{false};
std::atomic<bool> g_h_unready_seen{false};
std::atomic<bool> g_h_parent_ran{false};

class HandleChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(30ms); // Activating window
        co_return true;
    }
    void
    on(Tick &) {
        g_h_served.store(true); // the event pushed to id() while Activating, replayed after
    }
};

class HandleParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<HandleChild>();
        // The async child is Activating → handle is valid (id known) but NOT ready.
        g_h_unready_seen.store(child.valid() && !child.ready());
        push<Tick>(child.id(), 1); // safe: stashed until the child activates
        // Block until the child is active (poll-based, cancellation-aware).
        const bool ok = co_await child.ready_async(context(), 2s);
        g_h_ready_seen.store(ok && child.ready());
        if (child.ready())
            child->kill(); // deref-when-ready is now safe
        g_h_parent_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitHandle, ReadyAsyncAndIdStash) {
    g_h_served.store(false);
    g_h_ready_seen.store(false);
    g_h_unready_seen.store(false);
    g_h_parent_ran.store(false);
    qb::Main main;
    main.addActor<HandleParent>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_h_parent_ran.load());   // the parent's onInit ran all checks (no vacuous pass)
    EXPECT_TRUE(g_h_unready_seen.load());  // handle was valid-but-not-ready while Activating
    EXPECT_TRUE(g_h_ready_seen.load());    // ready_async resolved once the child activated
    EXPECT_TRUE(g_h_served.load());        // the id()-addressed event was stashed + replayed
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 2. Sync child: ready the instant addRefActor returns (no Activating phase).
// ===========================================================================
std::atomic<bool> g_sync_ready_now{false};
std::atomic<bool> g_sync_parent_ran{false};

class SyncChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true; // synchronous → active immediately
    }
};

class SyncParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<SyncChild>();
        g_sync_ready_now.store(child.ready()); // true at once — no Activating phase
        if (child.ready())
            child->kill();
        g_sync_parent_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitHandle, SyncChildReadyImmediately) {
    g_sync_ready_now.store(false);
    g_sync_parent_ran.store(false);
    qb::Main main;
    main.addActor<SyncParent>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_sync_parent_ran.load());
    EXPECT_TRUE(g_sync_ready_now.load()) << "a sync-init child must be ready() the instant addRefActor returns";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 3. A broadcast KillEvent reaches a still-Activating victim and cancels its init.
// ===========================================================================
std::atomic<bool> g_shut_started{false};
std::atomic<bool> g_shut_completed{false};
std::atomic<bool> g_shut_destroyed{false};

class ShutdownVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_shut_started.store(true);
        co_await context().sleep(500ms); // cancelled by the broadcast shutdown
        g_shut_completed.store(true);     // must NOT happen
        co_return true;
    }
    ~ShutdownVictim() override {
        g_shut_destroyed.store(true);
    }
};

class ShutdownTrigger : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        broadcast<qb::KillEvent>(); // a broadcast shutdown reaches the still-Activating victim
        kill();
        co_return true;
    }
};

TEST(InitHandle, BroadcastShutdownDuringAsyncOnInitCancelsCleanly) {
    g_shut_started.store(false);
    g_shut_completed.store(false);
    g_shut_destroyed.store(false);
    qb::Main main;
    main.addActor<ShutdownVictim>(0);
    main.addActor<ShutdownTrigger>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_shut_started.load());
    EXPECT_FALSE(g_shut_completed.load()); // init cancelled mid-flight
    EXPECT_TRUE(g_shut_destroyed.load());
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 4. ready_async returns false when the child never activates within the timeout.
// ===========================================================================
// A child that never activates within any test window (killed via broadcast at teardown).
class NeverReadyChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10s);
        co_return true;
    }
};

std::atomic<bool> g_ra_timedout{false};
std::atomic<bool> g_ra_parent_ran{false};

class WaitsThenTimesOut : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto       child = addRefActor<NeverReadyChild>();
        const bool ok    = co_await child.ready_async(context(), 80ms); // child never activates
        g_ra_timedout.store(!ok);
        g_ra_parent_ran.store(true);
        broadcast<qb::KillEvent>(); // tear down the stuck child + self
        kill();
        co_return true;
    }
};

TEST(InitHandle, ReadyAsyncTimesOutWhenChildNeverActivates) {
    g_ra_timedout.store(false);
    g_ra_parent_ran.store(false);
    qb::Main main;
    main.addActor<WaitsThenTimesOut>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_ra_parent_ran.load());
    EXPECT_TRUE(g_ra_timedout.load()) << "ready_async must return false when the child never activates";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 5. A parent killed while awaiting ready_async unwinds cleanly.
// ===========================================================================
std::atomic<bool> g_pk_destroyed{false};

class WaiterParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        addRefActor<NeverReadyChild>();
        auto child2 = addRefActor<NeverReadyChild>();
        co_await child2.ready_async(context(), 10s); // long wait — we are killed first
        co_return true;                               // unreachable
    }
    ~WaiterParent() override {
        g_pk_destroyed.store(true);
    }
};

class WaiterKiller : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(30ms);
        broadcast<qb::KillEvent>(); // kill the parent mid-ready_async + its stuck children
        kill();
        co_return true;
    }
};

TEST(InitHandle, ParentKilledWhileAwaitingReadyAsyncUnwinds) {
    g_pk_destroyed.store(false);
    qb::Main main;
    main.addActor<WaiterParent>(0);
    main.addActor<WaiterKiller>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_pk_destroyed.load()) << "a parent killed mid-ready_async must unwind cleanly";
    EXPECT_FALSE(main.hasError());
}

} // namespace
