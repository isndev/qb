/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/core/actor-customization-points.cpp
 * @brief Actor construction & identity customization points.
 *
 * Four orthogonal extension/identity contracts, each proven against the running engine:
 *   - `qb::no_default_events`  — opts an actor out of the four auto-registered handlers
 *      (KillEvent/SignalEvent/...). Proven by the *observable* consequence: such an actor
 *      ignores an external `push<KillEvent>` (no handler) yet still works via its own `kill()`,
 *      and re-registering KillEvent restores external killability.
 *   - `qb::allocate_actor<T>`  — a customization point routed through by BOTH the standard
 *      factory (`addActor`) and `addRefActor` (`VirtualCore::addReferencedActor`).
 *   - eager spawn counter       — a fresh actor exposes a live, 0-valued coroutine counter
 *      before any `spawn_detached`.
 *   - `qb::RefActorHandle<T>`   — validity vs liveness: resolves a live child, and `get()`
 *      returns nullptr once the child has `kill()`-ed (even while its object still lingers).
 *
 * In-actor `EXPECT_*`s are paired with a post-`join()` "ran" flag so a never-scheduled actor
 * cannot make the test pass vacuously.
 */

#include <atomic>
#include <cstddef>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

// =============================================================================
// no_default_events
// =============================================================================

namespace {

struct OptOutControlEvent : public qb::Event {};

std::atomic<bool> g_optout_self_kill_ran{false};
std::atomic<bool> g_optout_survived_external_kill{false};

// Opts out of default registrations and self-kills — proves kill() works without any handler.
class OptOutActor : public qb::Actor {
public:
    OptOutActor() : qb::Actor(qb::no_default_events) {}
    qb::io::async::task<bool>
    onInit() final {
        g_optout_self_kill_ran.store(true, std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

// Opts out, does NOT register KillEvent, stays alive on a control channel. An external
// push<KillEvent> has no handler → must be dropped (actor survives); the later control event
// then confirms the actor was still alive and tears it down.
class OptOutSurvivesKillActor : public qb::Actor {
public:
    OptOutSurvivesKillActor() : qb::Actor(qb::no_default_events) {}
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<OptOutControlEvent>(*this);
        co_return true;
    }
    void on(OptOutControlEvent const &) {
        // Reaching here proves the preceding KillEvent did NOT kill us.
        g_optout_survived_external_kill.store(true, std::memory_order_relaxed);
        kill();
    }
    // Deliberately NO on(KillEvent): an external KillEvent is unhandled → ignored.
};

// Opts out then opts back in to KillEvent → external push<KillEvent> kills it as usual.
class OptOutOptInKillActor : public qb::Actor {
public:
    OptOutOptInKillActor() : qb::Actor(qb::no_default_events) {}
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::KillEvent>(*this);
        co_return true;
    }
};

// Sends KillEvent (and a trailing control event) to a target, then self-kills.
class KillThenControlSender : public qb::Actor {
    const qb::ActorId _target;
    const bool        _also_control;

public:
    KillThenControlSender(qb::ActorId target, bool also_control)
        : _target(target)
        , _also_control(also_control) {}
    qb::io::async::task<bool>
    onInit() final {
        push<qb::KillEvent>(_target);
        if (_also_control)
            push<OptOutControlEvent>(_target); // FIFO: lands AFTER the (ignored) KillEvent
        kill();
        co_return true;
    }
};

} // namespace

TEST(NoDefaultEvents, ActorWithoutKillRegistrationStillSelfKills) {
    g_optout_self_kill_ran.store(false, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<OptOutActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_optout_self_kill_ran.load()) << "opt-out actor's own kill() must still work";
}

TEST(NoDefaultEvents, ExternalKillEventIsIgnoredWithoutHandler) {
    // Negative control: an opt-out actor with no KillEvent handler must SURVIVE an external
    // push<KillEvent> — only the subsequent control event tears it down.
    g_optout_survived_external_kill.store(false, std::memory_order_relaxed);
    qb::Main main;
    auto target = main.core(0).addActor<OptOutSurvivesKillActor>();
    main.core(0).addActor<KillThenControlSender>(target, /*also_control=*/true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_optout_survived_external_kill.load())
        << "no_default_events actor must ignore an unhandled external KillEvent";
}

TEST(NoDefaultEvents, OptInKillEventEnablesExternalKill) {
    qb::Main main;
    auto target = main.core(0).addActor<OptOutOptInKillActor>();
    main.core(0).addActor<KillThenControlSender>(target, /*also_control=*/false);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError()); // engine drains: re-registered KillEvent killed the target
}

// =============================================================================
// allocate_actor<T> customization point
// =============================================================================

namespace {

class CustomAllocActor;
inline std::atomic<std::size_t> custom_alloc_counter{0};

class CustomAllocActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

class CustomAllocRefParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        auto child = addRefActor<CustomAllocActor>(); // must also route through allocate_actor
        EXPECT_TRUE(child.valid());
        kill();
        co_return true;
    }
};

} // namespace

namespace qb {
// Full specialization of the allocate_actor customization point; both TActorFactory::create_impl
// (addActor) and VirtualCore::addReferencedActor (addRefActor) must route through it.
template <>
inline CustomAllocActor *
allocate_actor<CustomAllocActor>() {
    custom_alloc_counter.fetch_add(1, std::memory_order_relaxed);
    return new CustomAllocActor();
}
} // namespace qb

TEST(AllocateActor, IsRoutedFromStandardFactory) {
    custom_alloc_counter.store(0, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<CustomAllocActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(custom_alloc_counter.load(), 1u) << "allocate_actor<T> must be invoked by addActor's factory";
}

TEST(AllocateActor, IsRoutedFromAddRefActor) {
    custom_alloc_counter.store(0, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<CustomAllocRefParent>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(custom_alloc_counter.load(), 1u) << "allocate_actor<T> must also be invoked by addRefActor";
}

// =============================================================================
// eager spawn-detached coroutine counter
// =============================================================================

namespace {

std::atomic<bool> g_fresh_counter_checked{false};

class FreshCounterActor : public qb::Actor {
public:
    FreshCounterActor() {
        // Eager allocation: active_coroutines_ is a live shared_ptr the moment the ctor runs.
        EXPECT_FALSE(has_active_coroutines());
        EXPECT_EQ(active_coroutine_count(), 0u);
    }
    qb::io::async::task<bool>
    onInit() final {
        EXPECT_FALSE(has_active_coroutines());
        EXPECT_EQ(active_coroutine_count(), 0u);
        g_fresh_counter_checked.store(true, std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

} // namespace

TEST(SpawnAsync, CounterIsEagerlyAllocatedAndZero) {
    g_fresh_counter_checked.store(false, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<FreshCounterActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_fresh_counter_checked.load()) << "FreshCounterActor::onInit must have run";
}

// =============================================================================
// RefActorHandle<T> validity vs liveness
// =============================================================================

namespace {

std::atomic<bool> g_refhandle_test_completed{false};

class HandleChildActor : public qb::Actor {
public:
    int data = 42;
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

class HandleParentActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        // 1. Default-constructed handle is invalid.
        qb::RefActorHandle<HandleChildActor> empty;
        EXPECT_FALSE(empty.valid());
        EXPECT_EQ(empty.get(), nullptr);
        EXPECT_FALSE(static_cast<bool>(empty));

        // 2. Real handle to a live child resolves.
        auto handle = addRefHandle<HandleChildActor>();
        EXPECT_TRUE(handle.valid());
        EXPECT_NE(handle.get(), nullptr);
        EXPECT_EQ(handle->data, 42);
        EXPECT_TRUE(static_cast<bool>(handle));

        // 3. After the child kills itself, get() returns nullptr though the object still lingers
        //    in the _actors map until end-of-iteration; valid() reflects id validity, not liveness.
        handle->kill();
        EXPECT_EQ(handle.get(), nullptr) << "RefActorHandle::get() must return nullptr after child kill()";
        EXPECT_FALSE(static_cast<bool>(handle));
        EXPECT_TRUE(handle.valid());

        g_refhandle_test_completed.store(true, std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

} // namespace

TEST(RefActorHandle, ReportsNullptrAfterChildKill) {
    g_refhandle_test_completed.store(false, std::memory_order_relaxed);
    qb::Main main;
    main.core(0).addActor<HandleParentActor>();
    main.start(false);
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_refhandle_test_completed.load()) << "HandleParentActor::onInit must have run all checks";
}
