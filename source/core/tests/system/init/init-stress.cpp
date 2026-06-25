/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-stress.cpp
 * @brief Async-init at scale: many concurrent activations, dependency trees, and stash overflow.
 *
 * The Activating phase is a per-core map keyed by actor id; this file stresses it with breadth:
 *
 *   - MultipleActorsActivateConcurrently — three independent staggered inits all activate;
 *   - ManyConcurrentAsyncInits           — N=40 actors suspend then activate on one core (the map
 *                                          must track and drain all of them — exact count check);
 *   - TreeOfAsyncInitActors              — an async-initing parent spawns two async-initing children
 *                                          mid-init (a dependency tree: root + 2 leaves all activate);
 *   - StashOverflowFailsActivation       — a flood beyond `kActivationStashCap` (4096) forces the
 *                                          victim's activation to FAIL and removes it.
 *
 * tier=system. Every count is an EXACT post-`join()` assertion (no `> 0` looseness), so partial
 * activation or a silently dropped actor fails loudly. The stash-overflow case asserts the victim
 * was actually destroyed (the overflow forced a real failure), not merely that the engine survived.
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
// 1. A handful of independent staggered inits all activate concurrently.
// ===========================================================================
std::atomic<int> g_multi_activated{0};

class ConcurrentInitActor : public qb::Actor {
    int _ms;

public:
    explicit ConcurrentInitActor(int ms)
        : _ms(ms) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(_ms));
        g_multi_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(InitStress, MultipleActorsActivateConcurrently) {
    g_multi_activated.store(0);
    qb::Main main;
    main.addActor<ConcurrentInitActor>(0, 10);
    main.addActor<ConcurrentInitActor>(0, 25);
    main.addActor<ConcurrentInitActor>(0, 40);
    main.start(false);
    main.join();
    EXPECT_EQ(g_multi_activated.load(), 3);
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 2. Many concurrent async inits on one core — the Activating map drains all N.
// ===========================================================================
std::atomic<int> g_n_activated{0};

class OneShotAsync : public qb::Actor {
    int _ms;

public:
    explicit OneShotAsync(int ms)
        : _ms(ms) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(_ms));
        g_n_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(InitStress, ManyConcurrentAsyncInits) {
    g_n_activated.store(0);
    constexpr int N = 40;
    qb::Main      main;
    for (int i = 0; i < N; ++i)
        main.addActor<OneShotAsync>(0, 1 + (i % 30));
    main.start(false);
    main.join();
    EXPECT_EQ(g_n_activated.load(), N); // every one of the N activated — none stranded in the map
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 3. Dependency tree: an async-initing parent spawns two async-initing children
//    mid-init (root + 2 leaves all activate).
// ===========================================================================
std::atomic<int> g_tree_activated{0};

class TreeLeaf : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(15ms);
        g_tree_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

class TreeRoot : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Parent itself async-inits while spawning two async-init children.
        addRefActor<TreeLeaf>();
        co_await context().sleep(10ms);
        addRefActor<TreeLeaf>();
        g_tree_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(InitStress, TreeOfAsyncInitActors) {
    g_tree_activated.store(0);
    qb::Main main;
    main.addActor<TreeRoot>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_tree_activated.load(), 3); // root + 2 leaves all activated
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 4. Stash overflow: a flood beyond kActivationStashCap forces the activation to FAIL.
// ===========================================================================
std::atomic<bool> g_overflow_destroyed{false};
std::atomic<int>  g_overflow_handler_calls{0};

class StashOverflowVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(80ms); // stay Activating while the flood arrives
        co_return true;
    }
    void
    on(Tick &) {
        g_overflow_handler_calls.fetch_add(1); // never reached: the stash overflows + fails the init first
    }
    ~StashOverflowVictim() override {
        g_overflow_destroyed.store(true);
    }
};

class Flooder : public qb::Actor {
    qb::ActorId _t;

public:
    explicit Flooder(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 6000; ++i) // > kActivationStashCap (4096) → activation fails
            push<Tick>(_t, i);
        kill();
        co_return true;
    }
};

TEST(InitStress, StashOverflowFailsActivation) {
    g_overflow_destroyed.store(false);
    g_overflow_handler_calls.store(0);
    qb::Main   main;
    const auto victim = main.addActor<StashOverflowVictim>(0);
    main.addActor<Flooder>(0, victim);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_overflow_destroyed.load());     // overflow forced the activation to fail + remove it
    EXPECT_EQ(g_overflow_handler_calls.load(), 0); // the flood was never replayed (the actor failed init)
    EXPECT_FALSE(main.hasError());
}

} // namespace
