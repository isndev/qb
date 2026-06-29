/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-lifecycle.cpp
 * @brief The async-`onInit()` activation state machine — the core happy/fail paths.
 *
 * `qb::Actor::onInit()` is a coroutine returning `qb::io::async::task<bool>`. An init that
 * `co_await`s suspends and the actor enters the *Activating* phase: `is_active()` is false,
 * the owning core keeps serving its other actors, and the frame is owned by the core (deferred
 * destroy) until it resumes to completion. This file proves the state-machine transitions:
 *
 *   - suspend-then-complete  → `co_await` then `co_return true` activates the actor;
 *   - kill-during-init       → a unicast `KillEvent` reaches an Activating actor, cancels the
 *                              coroutine (it never completes), and the actor outlives its own
 *                              frame before being destroyed cleanly (deferred destroy);
 *   - async fail             → `co_return false` after a suspension removes the actor;
 *   - sync fail (no co_await) → `co_return false` and a thrown exception both fail the init on the
 *                              SYNCHRONOUS path, mirroring their suspended kin, and (as the initial
 *                              actor of a core) abort `start()` with `hasError()`;
 *   - multi-suspension        → a chain of three `co_await`s all run, in order;
 *   - disabled deadline       → `activation_deadline_ns == 0` never force-fails a slow-but-fine init.
 *
 * All `tier=system` (real `qb::Main`, real event loop, deferred-destroy machinery). Every in-actor
 * side-effect is mirrored to a post-`join()` atomic so a never-scheduled actor cannot pass vacuously.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites (the coroutine
 * frame pool is deliberately not drained at thread exit).
 */

#include <atomic>
#include <chrono>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::ScopedDeadline;

namespace {

// ---------------------------------------------------------------------------
// 1. An onInit that co_awaits completes, then the actor activates.
// ---------------------------------------------------------------------------
std::atomic<bool> g_completed{false};

class AwaitThenRunActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // suspends → Activating; core keeps serving
        g_completed.store(true);
        kill(); // nothing else to do — let the engine drain
        co_return true;
    }
};

TEST(InitLifecycle, AwaitingOnInitCompletesAndActivates) {
    g_completed.store(false);
    qb::Main main;
    main.addActor<AwaitThenRunActor>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_completed.load()) << "the suspended onInit must resume to completion";
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 2. Killing an actor mid-async-init cancels the coroutine and the actor is
//    destroyed cleanly (deferred destroy: it outlives its own onInit frame).
// ---------------------------------------------------------------------------
std::atomic<bool> g_started{false};
std::atomic<bool> g_init_finished{false};
std::atomic<bool> g_destroyed{false};

class LongInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_started.store(true);
        co_await context().sleep(500ms); // cancelled by the kill long before this elapses
        g_init_finished.store(true);     // must NOT be reached
        co_return true;
    }
    ~LongInitActor() override {
        g_destroyed.store(true);
    }
};

class KillerActor : public qb::Actor {
    qb::ActorId _target;

public:
    explicit KillerActor(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // let LongInitActor enter its long init first
        push<qb::KillEvent>(_target);   // unicast kill must reach an Activating actor
        kill();
        co_return true;
    }
};

TEST(InitLifecycle, KillDuringInitCancelsAndDestroysCleanly) {
    g_started.store(false);
    g_init_finished.store(false);
    g_destroyed.store(false);

    qb::Main   main;
    const auto target = main.addActor<LongInitActor>(0);
    main.addActor<KillerActor>(0, target);
    main.start(false);
    main.join();

    EXPECT_TRUE(g_started.load());        // the init body ran up to its first co_await
    EXPECT_FALSE(g_init_finished.load()); // ...but was cancelled, never completed
    EXPECT_TRUE(g_destroyed.load());      // actor torn down after its frame unwound (deferred destroy)
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 3. An async onInit that co_returns false AFTER suspending removes the actor.
// ---------------------------------------------------------------------------
std::atomic<bool> g_asyncfail_destroyed{false};

class AsyncFailActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(15ms);
        co_return false; // initialization failed after suspending → actor removed
    }
    ~AsyncFailActor() override {
        g_asyncfail_destroyed.store(true);
    }
};

TEST(InitLifecycle, AsyncInitFailureRemovesActor) {
    g_asyncfail_destroyed.store(false);
    qb::Main main;
    main.addActor<AsyncFailActor>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_asyncfail_destroyed.load()) << "a suspended co_return false must remove the actor";
}

// ---------------------------------------------------------------------------
// 4. SYNCHRONOUS-path outcomes (no co_await): co_return false and a thrown
//    exception must both fail the init exactly like their suspended kin. As the
//    initial actor of a core, a failed sync init aborts start() with hasError().
// ---------------------------------------------------------------------------
std::atomic<bool> g_syncfalse_destroyed{false};

class SyncFalseInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false; // completes synchronously → __drive_init__ ReadyFalse
    }
    ~SyncFalseInit() override {
        g_syncfalse_destroyed.store(true);
    }
};

TEST(InitLifecycle, SyncOnInitReturnsFalseWithoutCoAwait) {
    g_syncfalse_destroyed.store(false);
    qb::Main main;
    main.addActor<SyncFalseInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());              // initial-actor sync init failure aborts start
    EXPECT_TRUE(g_syncfalse_destroyed.load()); // actor removed
}

std::atomic<bool> g_syncthrow_destroyed{false};

class SyncThrowInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        if (id().is_valid()) // always true; defeats unreachable-code analysis on the co_return
            throw std::runtime_error("init blew up synchronously");
        co_return true;
    }
    ~SyncThrowInit() override {
        g_syncthrow_destroyed.store(true);
    }
};

TEST(InitLifecycle, SyncOnInitThrowsWithoutCoAwait) {
    g_syncthrow_destroyed.store(false);
    qb::Main main;
    main.addActor<SyncThrowInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError()); // uncaught sync throw ⇒ init failure aborts start
    EXPECT_TRUE(g_syncthrow_destroyed.load());
}

// ---------------------------------------------------------------------------
// 5. A chain of co_awaits inside onInit: every suspension resumes, in order.
// ---------------------------------------------------------------------------
std::atomic<int> g_chain_steps{0};

class MultiSuspendInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(InitLifecycle, MultiSuspensionChain) {
    g_chain_steps.store(0);
    qb::Main main;
    main.addActor<MultiSuspendInit>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_chain_steps.load(), 3) << "all three suspensions must resume in order";
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 6. An exception thrown AFTER a suspension fails the init (mirrors the sync throw).
// ---------------------------------------------------------------------------
std::atomic<bool> g_throw_started{false};
std::atomic<bool> g_throw_destroyed{false};

class ThrowingInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10ms);
        g_throw_started.store(true);
        throw std::runtime_error("init blew up after a suspension");
        co_return true; // unreachable
    }
    ~ThrowingInit() override {
        g_throw_destroyed.store(true);
    }
};

TEST(InitLifecycle, ExceptionAfterSuspensionFailsInit) {
    g_throw_started.store(false);
    g_throw_destroyed.store(false);
    qb::Main main;
    main.addActor<ThrowingInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_throw_started.load());   // reached the throw site after resuming
    EXPECT_TRUE(g_throw_destroyed.load()); // uncaught throw → init failed → actor removed
}

// ---------------------------------------------------------------------------
// 7. A disabled activation deadline (activation_deadline_ns == 0) never force-fails
//    an Activating actor — a slow-but-fine init still completes naturally.
// ---------------------------------------------------------------------------
std::atomic<bool> g_slowfine_activated{false};

class SlowButFineInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(60ms);
        g_slowfine_activated.store(true);
        kill();
        co_return true;
    }
};

TEST(InitLifecycle, DisabledDeadlineNeverTimesOutActivating) {
    g_slowfine_activated.store(false);
    ScopedDeadline dl(0); // disable the deadline
    qb::Main       main;
    main.addActor<SlowButFineInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_slowfine_activated.load()) << "a disabled deadline must let a slow init finish naturally";
    EXPECT_FALSE(main.hasError());
}

} // namespace
