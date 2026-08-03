/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/actor/actor-dependency.cpp
 * @brief Actor-discovery: the three ways one actor learns another's `ActorId`.
 *
 * Pins, on a real `qb::Main`, the three dependency-resolution paths an actor uses to find peers:
 *   1. ids collected from each `main.addActor<>()` return value;
 *   2. ids collected from a `core(0).builder()...idList()`;
 *   3. RUNTIME discovery via `require<TestActor>()` → a broadcast `PingEvent` that every live
 *      `TestActor` answers with a `RequireEvent` (matched by `is<TestActor>(event)`).
 *
 * The discovery contract is presence-IS-status: a `require<T>()` ping is answered ONLY by live
 * actors of `T`, so the number of `RequireEvent` replies equals the number of live `T`. The
 * positive cases therefore assert the discoverer counted EXACTLY `MAX_ACTOR` replies (surfaced to a
 * post-`join()` atom), not merely that the engine drained without error. A negative case proves the
 * other half of the contract: `require<T>()` for a `T` with ZERO live instances resolves cleanly —
 * the discoverer receives no replies, does not hang, and a self-sent control event (still
 * registered) proves it stayed alive and drained its mailbox, so a discovered-count of 0 means
 * "no live dependency", not "never scheduled".
 *
 * No wall-clock oracle: the discoverer self-kills the instant its expected reply quota arrives (or,
 * in the negative case, when its control event lands), and the engine drains. The ctest TIMEOUT is
 * the only backstop — a genuine hang (e.g. require<> wedging) fails loudly there.
 */

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

constexpr std::uint32_t MAX_ACTOR = 2048;

// Discovered-reply count, mirrored from the discoverer to the test body (reset per test).
std::atomic<std::uint32_t> g_discovered{0};
std::atomic<bool>          g_control_seen{false}; // negative case: the discoverer drained its mailbox

class TestActor : public qb::Actor {
public:
    TestActor() = default;
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

// A different actor type with no live instances in the negative case — the require<> target there.
class AbsentActor : public qb::Actor {
public:
    AbsentActor() = default;
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

class TestActorDependency : public qb::Actor {
    qb::ActorIdList const _ids;

public:
    explicit TestActorDependency(qb::ActorIdList const &ids = {})
        : _ids(ids) {
        if (_ids.empty()) {
            registerEvent<qb::RequireEvent>(*this);
            require<TestActor>(); // broadcast a typed PingEvent; live TestActors reply
        } else {
            for (auto id : _ids)
                push<qb::KillEvent>(id);
            kill();
        }
    }

    std::uint32_t counter = 0;

    void
    on(qb::RequireEvent const &event) {
        if (is<TestActor>(event)) {
            ++counter;
            g_discovered.store(counter, std::memory_order_relaxed);
            send<qb::KillEvent>(event.getSource());
        }
        if (counter == MAX_ACTOR)
            kill();
    }
};

// Self-sent control probe — proves the discoverer is alive and draining its mailbox even when no
// RequireEvent reply ever arrives (the negative case).
struct ControlProbe : public qb::Event {};

// Discoverer that requires a type (`AbsentActor`) for which NO live actor exists. It must receive
// zero RequireEvent replies, yet its self-sent ControlProbe must land — proving liveness.
class AbsentDependencyActor : public qb::Actor {
public:
    AbsentDependencyActor() {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<ControlProbe>(*this);
        require<AbsentActor>();   // nobody matches → no reply expected
        push<ControlProbe>(id()); // ordered AFTER the require broadcast: confirms liveness
    }

    void
    on(qb::RequireEvent const &event) {
        if (is<AbsentActor>(event))
            g_discovered.fetch_add(1, std::memory_order_relaxed); // must never fire
    }

    void
    on(ControlProbe const &) {
        g_control_seen.store(true); // discoverer reached its own control event → alive, mailbox drained
        kill();
    }
};

void
reset_atoms() {
    g_discovered.store(0, std::memory_order_relaxed);
    g_control_seen.store(false);
}

// ===========================================================================
// Positive: every discovery path resolves EXACTLY MAX_ACTOR peers.
// ===========================================================================

TEST(ActorDependency, GetActorIdDependencyFromAddActorAtStart) {
    reset_atoms();
    qb::Main main;

    qb::ActorIdList list;
    for (auto i = 0u; i < MAX_ACTOR; ++i)
        list.push_back(main.addActor<TestActor>(0));
    main.addActor<TestActorDependency>(1, list);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdDependencyFromCoreBuilderAtStart) {
    reset_atoms();
    qb::Main main;

    auto builder = main.core(0).builder();
    for (auto i = 0u; i < MAX_ACTOR; ++i)
        builder.addActor<TestActor>();
    main.addActor<TestActorDependency>(1, builder.idList());

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorDependency, GetActorIdDependencyFromRequireEvent) {
    reset_atoms();
    qb::Main main;

    auto builder = main.core(0).builder();
    for (auto i = 0u; i < MAX_ACTOR; ++i)
        builder.addActor<TestActor>();
    main.addActor<TestActorDependency>(1);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    // The discoverer must have seen EXACTLY one RequireEvent reply per live TestActor.
    EXPECT_EQ(g_discovered.load(), MAX_ACTOR) << "require<TestActor>() must resolve every one of the " << MAX_ACTOR << " live actors";
}

// ===========================================================================
// Negative: require<> for a type with zero live actors resolves to nothing, cleanly.
// ===========================================================================

TEST(ActorDependency, RequireWithNoMatchingActorsResolvesToZeroAndDoesNotHang) {
    reset_atoms();
    qb::Main main;

    // The discoverer is the sole actor: it require<AbsentActor>()s a type with ZERO live instances.
    // No reply can ever arrive; only its own ControlProbe lands, after which it self-kills and the
    // engine drains. (A genuine require<> wedge would hang and trip the ctest TIMEOUT.)
    main.addActor<AbsentDependencyActor>(0);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_control_seen.load()) << "the discoverer must have drained its mailbox (control event landed) — not a never-scheduled pass";
    EXPECT_EQ(g_discovered.load(), 0u) << "require<AbsentActor>() must resolve zero peers: no live AbsentActor exists";
}

} // namespace
