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
#include <memory>
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

// Startup failure leaves this actor owned by the core. Its destructor runs on
// the core thread and may use the same context as a normally reaped actor.
std::atomic<qb::CoreId> g_failure_destructor_core{qb::MaxCores};

class CoreAwareFailedInit final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false;
    }

    ~CoreAwareFailedInit() final {
        g_failure_destructor_core.store(getIndex(), std::memory_order_relaxed);
    }
};

TEST(InitLifecycle, FailedStartupKeepsCoreAvailableToActorDestructor) {
    g_failure_destructor_core.store(qb::MaxCores, std::memory_order_relaxed);
    qb::Main main;
    main.addActor<CoreAwareFailedInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_failure_destructor_core.load(std::memory_order_relaxed), 0);
}

std::atomic<int> g_queued_payload_destroyed{0};

struct CountPayloadDeletion {
    void
    operator()(int *ptr) const noexcept {
        ++g_queued_payload_destroyed;
        delete ptr;
    }
};

struct QueuedOwnedEvent final : qb::Event {
    std::unique_ptr<int, CountPayloadDeletion> payload;

    explicit QueuedOwnedEvent(std::unique_ptr<int, CountPayloadDeletion> value)
        : payload(std::move(value)) {}
};

class QueuesBeforeFailedInit final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        push<QueuedOwnedEvent>(id(), std::unique_ptr<int, CountPayloadDeletion>(new int(1)));
        co_return false;
    }
};

TEST(InitLifecycle, FailedStartupDisposesQueuedSelfEventPayload) {
    g_queued_payload_destroyed.store(0);
    qb::Main main;
    main.addActor<QueuesBeforeFailedInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_queued_payload_destroyed.load(), 1);
}

std::atomic<qb::CoreId> g_tick_failure_destructor_core{qb::MaxCores};
std::atomic<int>        g_tick_calls{0};

class ThrowsFromTick final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) override {
        ++g_tick_calls;
        push<QueuedOwnedEvent>(id(), std::unique_ptr<int, CountPayloadDeletion>(new int(2)));
        throw std::runtime_error("tick failed after owning an event");
    }

    ~ThrowsFromTick() final {
        g_tick_failure_destructor_core.store(getIndex(), std::memory_order_relaxed);
    }
};

TEST(InitLifecycle, ThrowingTickTearsDownActorAndQueuedPayload) {
    g_tick_calls.store(0);
    g_tick_failure_destructor_core.store(qb::MaxCores, std::memory_order_relaxed);
    g_queued_payload_destroyed.store(0);
    qb::Main main;
    main.addActor<ThrowsFromTick>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_tick_calls.load(), 1);
    EXPECT_EQ(g_tick_failure_destructor_core.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(g_queued_payload_destroyed.load(), 1);
}

constexpr int    kCancelHookChildren = 512;
std::atomic<int> g_cancel_hook_spawned{0};
std::atomic<int> g_cancel_hook_valid{0};
std::atomic<int> g_cancel_hook_children_constructed{0};
std::atomic<int> g_cancel_hook_children_destroyed{0};
std::atomic<int> g_cancel_hook_parent_destroyed{0};
std::atomic<int> g_cancel_hook_sibling_destroyed{0};

class CancelHookChild final : public qb::Actor {
public:
    CancelHookChild() {
        ++g_cancel_hook_children_constructed;
    }
    ~CancelHookChild() final {
        ++g_cancel_hook_children_destroyed;
    }
};

class CancelHookSibling final : public qb::Actor {
public:
    ~CancelHookSibling() final {
        ++g_cancel_hook_sibling_destroyed;
    }
};

class SpawnsChildrenWhenCancelled final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto ctx = context();
        ctx.token().on_cancel([this] {
            for (int i = 0; i < kCancelHookChildren; ++i) {
                auto child = addRefActor<CancelHookChild>(); // grows _actors while terminal cancellation runs
                ++g_cancel_hook_spawned;
                if (child.valid())
                    ++g_cancel_hook_valid;
            }
        });
        co_return false;
    }
    ~SpawnsChildrenWhenCancelled() final {
        ++g_cancel_hook_parent_destroyed;
    }
};

TEST(InitLifecycle, TerminalCancellationMayAddActors) {
    g_cancel_hook_spawned              = 0;
    g_cancel_hook_valid                = 0;
    g_cancel_hook_children_constructed = 0;
    g_cancel_hook_children_destroyed   = 0;
    g_cancel_hook_parent_destroyed     = 0;
    g_cancel_hook_sibling_destroyed    = 0;
    qb::Main main;
    main.addActor<SpawnsChildrenWhenCancelled>(0);
    main.addActor<CancelHookSibling>(0); // next slot forces the invalidated iterator to advance
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_cancel_hook_spawned.load(), kCancelHookChildren);
    EXPECT_EQ(g_cancel_hook_valid.load(), 0);
    EXPECT_EQ(g_cancel_hook_children_constructed.load(), 0);
    EXPECT_EQ(g_cancel_hook_children_destroyed.load(), 0);
    EXPECT_EQ(g_cancel_hook_parent_destroyed.load(), 1);
    EXPECT_EQ(g_cancel_hook_sibling_destroyed.load(), 1);
}

constexpr int    kStashDestroyChildrenPerEvent = 256;
std::atomic<int> g_stash_events_destroyed{0};
std::atomic<int> g_stash_children_valid{0};
std::atomic<int> g_stash_children_constructed{0};
std::atomic<int> g_stash_children_destroyed{0};
std::atomic<int> g_stash_targets_destroyed{0};
std::atomic<int> g_stash_owner_destroyed{0};

class StashDestroyOwner;

struct SpawnFromStashEvent final : qb::Event {
    StashDestroyOwner *owner;
    explicit SpawnFromStashEvent(StashDestroyOwner *actor)
        : owner(actor) {}
    ~SpawnFromStashEvent();
};

class StashDestroyChild final : public qb::Actor {
public:
    StashDestroyChild() {
        ++g_stash_children_constructed;
    }
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10s); // would insert into _activating during teardown
        co_return true;
    }
    ~StashDestroyChild() final {
        ++g_stash_children_destroyed;
    }
};

class StashDestroyTarget final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SpawnFromStashEvent>(*this);
        co_await context().sleep(10s); // keeps inbound business events in the stash
        co_return true;
    }
    void
    on(SpawnFromStashEvent const &) {}
    ~StashDestroyTarget() final {
        ++g_stash_targets_destroyed;
    }
};

class StashDestroyOwner final
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _first;
    qb::ActorId _second;

public:
    StashDestroyOwner(qb::ActorId first, qb::ActorId second)
        : _first(first)
        , _second(second) {}
    ~StashDestroyOwner() final {
        ++g_stash_owner_destroyed;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        push<SpawnFromStashEvent>(_first, this);
        push<SpawnFromStashEvent>(_second, this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        throw std::runtime_error("stop with two activation stashes");
    }

    void
    spawn_children() {
        for (int i = 0; i < kStashDestroyChildrenPerEvent; ++i)
            if (addRefActor<StashDestroyChild>().valid())
                ++g_stash_children_valid;
    }
};

SpawnFromStashEvent::~SpawnFromStashEvent() {
    ++g_stash_events_destroyed;
    owner->spawn_children();
}

TEST(InitLifecycle, StashPayloadDestructionMayAddActivations) {
    g_stash_events_destroyed     = 0;
    g_stash_children_valid       = 0;
    g_stash_children_constructed = 0;
    g_stash_children_destroyed   = 0;
    g_stash_targets_destroyed    = 0;
    g_stash_owner_destroyed      = 0;
    qb::Main main;
    auto     first  = main.addActor<StashDestroyTarget>(0);
    auto     second = main.addActor<StashDestroyTarget>(0);
    main.addActor<StashDestroyOwner>(0, first, second);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_stash_events_destroyed.load(), 2);
    EXPECT_EQ(g_stash_children_valid.load(), 0);
    EXPECT_EQ(g_stash_children_constructed.load(), 0);
    EXPECT_EQ(g_stash_children_destroyed.load(), 0);
    EXPECT_EQ(g_stash_targets_destroyed.load(), 2);
    EXPECT_EQ(g_stash_owner_destroyed.load(), 1);
}

std::atomic<int> g_remove_hook_children_valid{0};
std::atomic<int> g_remove_hook_children_destroyed{0};
std::atomic<int> g_remove_hook_parent_destroyed{0};

class RemoveHookChild final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        kill();
        co_return true;
    }
    ~RemoveHookChild() final {
        ++g_remove_hook_children_destroyed;
    }
};

class AsyncFalseSpawnsOnCancel final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto ctx = context();
        ctx.token().on_cancel([this] {
            for (int i = 0; i < kCancelHookChildren; ++i)
                if (addRefActor<RemoveHookChild>().valid())
                    ++g_remove_hook_children_valid;
        });
        co_await ctx.sleep(5ms);
        co_return false; // __pump_activations__ removes this actor outside terminal teardown
    }
    ~AsyncFalseSpawnsOnCancel() final {
        ++g_remove_hook_parent_destroyed;
    }
};

TEST(InitLifecycle, RemovingFailedAsyncActorMayGrowRegistryDuringCancel) {
    g_remove_hook_children_valid     = 0;
    g_remove_hook_children_destroyed = 0;
    g_remove_hook_parent_destroyed   = 0;
    qb::Main main;
    main.addActor<AsyncFalseSpawnsOnCancel>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_remove_hook_children_valid.load(), kCancelHookChildren);
    EXPECT_EQ(g_remove_hook_children_destroyed.load(), kCancelHookChildren);
    EXPECT_EQ(g_remove_hook_parent_destroyed.load(), 1);
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

namespace frame_teardown_test {

constexpr int    kChildren = 256;
std::atomic<int> g_frame_local_destroyed{0};
std::atomic<int> g_children_valid{0};
std::atomic<int> g_children_constructed{0};
std::atomic<int> g_children_destroyed{0};
std::atomic<int> g_spawner_missing{0};

class Child final : public qb::Actor {
public:
    Child() {
        ++g_children_constructed;
    }
    qb::io::async::task<bool>
    onInit() override {
        co_await qb::io::async::sleep(10s); // registers a scheduler-suspended frame
        co_return true;
    }
    ~Child() final {
        ++g_children_destroyed;
    }
};

class Spawner final : public qb::Actor {
public:
    void
    spawn_children() {
        for (int i = 0; i < kChildren; ++i)
            if (addRefActor<Child>().valid())
                ++g_children_valid;
    }
};

struct FrameLocal {
    qb::ActorHandle<Spawner> spawner;
    ~FrameLocal() {
        ++g_frame_local_destroyed;
        if (auto *actor = spawner.get())
            actor->spawn_children();
        else
            ++g_spawner_missing;
    }
};

class ThrowsWithParkedFrame final
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto spawner = addRefActor<Spawner>();
        spawn([spawner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            FrameLocal local{spawner};
            co_await ctx.sleep(10s);
        });
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        throw std::runtime_error("stop with parked frame");
    }
};

TEST(InitLifecycle, SchedulerFrameDestructionCannotAdmitNewAsyncActors) {
    g_frame_local_destroyed = 0;
    g_children_valid        = 0;
    g_children_constructed  = 0;
    g_children_destroyed    = 0;
    g_spawner_missing       = 0;
    qb::Main main;
    main.addActor<ThrowsWithParkedFrame>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());
    EXPECT_EQ(g_frame_local_destroyed.load(), 1);
    EXPECT_EQ(g_spawner_missing.load(), 0);
    EXPECT_EQ(g_children_valid.load(), 0);
    EXPECT_EQ(g_children_constructed.load(), 0);
    EXPECT_EQ(g_children_destroyed.load(), 0);
}

} // namespace frame_teardown_test

namespace activation_rehash_test {

constexpr int    kChildrenPerHook = 256;
std::atomic<int> g_hooks{0};
std::atomic<int> g_children_valid{0};
std::atomic<int> g_children_destroyed{0};
std::atomic<int> g_parents_destroyed{0};

class Child final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(1ms); // inserts into _activating during the deadline walk
        kill();
        co_return true;
    }
    ~Child() final {
        ++g_children_destroyed;
    }
};

class DeadlineParent final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto ctx = context();
        ctx.token().on_cancel([this] {
            ++g_hooks;
            for (int i = 0; i < kChildrenPerHook; ++i)
                if (addRefActor<Child>().valid())
                    ++g_children_valid;
        });
        co_await ctx.sleep(10s);
        co_return true;
    }
    ~DeadlineParent() final {
        ++g_parents_destroyed;
    }
};

TEST(InitLifecycle, DeadlineCancellationMayInsertActivations) {
    g_hooks              = 0;
    g_children_valid     = 0;
    g_children_destroyed = 0;
    g_parents_destroyed  = 0;
    ScopedDeadline dl(50'000'000); // long inits expire; children complete in 1 ms
    qb::Main       main;
    main.addActor<DeadlineParent>(0);
    main.addActor<DeadlineParent>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_hooks.load(), 2);
    EXPECT_EQ(g_children_valid.load(), 2 * kChildrenPerHook);
    EXPECT_EQ(g_children_destroyed.load(), 2 * kChildrenPerHook);
    EXPECT_EQ(g_parents_destroyed.load(), 2);
}

} // namespace activation_rehash_test
