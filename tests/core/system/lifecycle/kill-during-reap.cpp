/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/kill-during-reap.cpp
 * @brief An actor killed from another actor's destructor must still be reaped.
 *
 * `VirtualCore::__workflow__` reaps dead actors with
 *
 * @code
 * for (auto const &id : _actor_to_remove) removeActor(id);
 * _actor_to_remove.clear();
 * if (_actors.empty()) break;
 * @endcode
 *
 * `removeActor` destroys the actor, which runs arbitrary user code. If that code calls
 * `kill()` on a *different* actor it re-enters `VirtualCore::killActor`, which does
 * `_actor_to_remove.insert(id)` — **on the container currently being range-iterated**. Two
 * distinct hazards follow:
 *
 *  1. `RemoveActorList` is `qb::unordered_set<ActorId>`. Under NDEBUG that is the ska flat
 *     hash set, where an insert that grows the table reallocates the entry array and
 *     invalidates every live iterator (the sanitizer presets use node-based
 *     `std::unordered_set` and structurally cannot see it — the release-only class from the
 *     earlier deep audit).
 *  2. Even setting iterator validity aside, an id inserted *behind* the iterator is never
 *     visited, and the unconditional `clear()` then discards it. The victim is left
 *     `!is_alive()` (so it receives nothing) yet still present in `_actors`, so
 *     `_actors.empty()` never becomes true and the core never leaves `__workflow__` —
 *     `qb::Main::join()` hangs.
 *
 * Group A is killed by a broadcast; every A destructor kills its paired B. B registers no
 * event handler, so it is reachable *only* through that destructor — the test fails unless
 * inserts made during the reap loop are honoured. The engine runs on a detached thread behind
 * a watchdog so a regression fails this test instead of hanging the whole suite.
 */

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

constexpr std::size_t kPairs = 512; ///< large enough to force several growth rehashes

std::atomic<int> g_a_destroyed{0};
std::atomic<int> g_b_destroyed{0};

/// Raw pointers to the victims, in creation order. Written by victim constructors and read by
/// killer destructors — both run on core 0's own thread, so no synchronisation is needed.
std::vector<qb::Actor *> g_victims;

struct GoEvent : public qb::Event {};

/// Registers no handler: unreachable by any event, killable only from a KillerActor destructor.
class VictimActor final : public qb::Actor {
public:
    const std::size_t _slot;

    VictimActor()
        : _slot(g_victims.size()) {
        g_victims.push_back(this);
    }
    ~VictimActor() final {
        // Clear our own slot so a killer destructor can never observe a dangling pointer —
        // teardown destroys `_actors` in unspecified order, so a killer may well outlive its
        // victim. Without this the test would carry its own use-after-free and could not
        // attribute a crash to the framework.
        g_victims[_slot] = nullptr;
        g_b_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

/// Killed by the broadcast; kills its paired victim from its DESTRUCTOR.
class KillerActor final : public qb::Actor {
    const std::size_t _victim_index;

public:
    explicit KillerActor(std::size_t victim_index)
        : _victim_index(victim_index) {}

    ~KillerActor() final {
        g_a_destroyed.fetch_add(1, std::memory_order_relaxed);
        // Re-enters VirtualCore::killActor -> _actor_to_remove.insert() while the reap loop
        // is range-iterating that exact container.
        if (_victim_index < g_victims.size()) {
            auto *victim = g_victims[_victim_index];
            if (victim != nullptr && victim->is_alive())
                victim->kill();
        }
    }

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<GoEvent>(*this);
        co_return true;
    }
    void
    on(GoEvent const &) {
        kill();
    }
};

/// One broadcast puts every KillerActor into _actor_to_remove in the same pass.
class TriggerActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        broadcast<GoEvent>();
        kill();
        co_return true;
    }
};

/// Runs the engine on a detached thread; false if it failed to terminate within `budget`.
[[nodiscard]] bool
run_engine(std::chrono::seconds budget) {
    auto done   = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    std::thread([done] {
        qb::Main main;
        // Victims are registered first so every g_victims slot exists by the time the
        // matching killer's destructor runs.
        for (std::size_t i = 0; i < kPairs; ++i)
            main.addActor<VictimActor>(0);
        for (std::size_t i = 0; i < kPairs; ++i)
            main.addActor<KillerActor>(0, i);
        main.addActor<TriggerActor>(0);
        main.start(false);
        main.join();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

} // namespace

TEST(KillDuringReap, ActorKilledFromAnotherDestructorIsStillReaped) {
    g_a_destroyed = 0;
    g_b_destroyed = 0;
    g_victims.clear();
    g_victims.reserve(kPairs);

    ASSERT_TRUE(run_engine(std::chrono::seconds(30))) << "engine did not terminate: an actor killed from another actor's destructor was "
                                                         "inserted into _actor_to_remove behind the reap iterator, then discarded by the "
                                                         "unconditional clear() — it stays in _actors forever, so _actors.empty() never holds";

    EXPECT_EQ(g_a_destroyed.load(), static_cast<int>(kPairs));
    EXPECT_EQ(g_b_destroyed.load(), static_cast<int>(kPairs)) << "every victim killed from a destructor must still be reaped";
}
