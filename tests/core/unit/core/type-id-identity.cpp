/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/type-id-identity.cpp
 * @brief `qb::type_id<T>` / `qb::Event::type_to_id<T>` identity contract.
 *
 * Every distinct type maps to a dense, stable, collision-free id that the event router uses as
 * its routing key. The id is produced by a magic-static post-increment counter, so it must be:
 *   - stable: same type → same id, forever (IsStableAcrossCalls);
 *   - collision-free across a large flat type cohort (IsCollisionFreeAcrossManyTypes);
 *   - race-free on first instantiation from many threads (ConcurrentFirstInstantiationIsRaceFree);
 *   - the same id the Event layer keys on (EventTypeToIdMatchesGlobalTypeId).
 *
 * Pure logic — no engine, no thread spawned beyond the explicit race probe. Promoted to unit/
 * from the former test-core-improvements.cpp gold-standard suite.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>

namespace {

// 256 distinct types via a non-type template parameter — stresses uniqueness across a large cohort.
template <std::size_t N>
struct TypeIdProbe {};

template <std::size_t... Is>
void
collectTypeIds(std::index_sequence<Is...>, std::vector<qb::TypeId> &out) {
    (out.push_back(qb::type_id<TypeIdProbe<Is>>()), ...);
}

// Disjoint cohort for the concurrent test — keeps the optimizer from sharing an instantiation.
template <std::size_t N>
struct ConcurrentTypeIdProbe {};

} // namespace

TEST(TypeId, IsStableAcrossCalls) {
    const auto a = qb::type_id<int>();
    for (std::size_t i = 0; i < 1024; ++i)
        EXPECT_EQ(qb::type_id<int>(), a);
    const auto b = qb::type_id<double>();
    EXPECT_NE(a, b) << "Distinct types must not collide";
}

TEST(TypeId, IsCollisionFreeAcrossManyTypes) {
    constexpr std::size_t   kNTypes = 256;
    std::vector<qb::TypeId> ids;
    ids.reserve(kNTypes);
    collectTypeIds(std::make_index_sequence<kNTypes>{}, ids);

    // All ids unique up to numeric_limits<TypeId>::max() distinct types.
    std::unordered_set<qb::TypeId> uniq(ids.begin(), ids.end());
    EXPECT_EQ(uniq.size(), ids.size()) << "type_id<T>() collided over " << kNTypes << " types";
    for (auto id : ids)
        EXPECT_NE(id, qb::TypeId{0}) << "TypeId 0 is reserved as 'unassigned'";
}

TEST(TypeId, ConcurrentFirstInstantiationIsRaceFree) {
    // Many threads call type_id<T>() for the same T at once: the magic-static initialiser barrier
    // in detail::type_id_for<T> must serialise the post-increment so all threads see ONE id.
    constexpr std::size_t            kThreads = 16;
    std::atomic<bool>                go{false};
    std::array<qb::TypeId, kThreads> seen{};
    std::vector<std::thread>         workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &go, &seen] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            seen[i] = qb::type_id<ConcurrentTypeIdProbe<999>>();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : workers)
        t.join();
    for (std::size_t i = 1; i < kThreads; ++i)
        EXPECT_EQ(seen[i], seen[0]) << "thread " << i << " observed a different id";
}

TEST(TypeId, EventTypeToIdMatchesGlobalTypeId) {
    // Event::type_to_id<T>() keys the router; its representation is build-mode dependent by design:
    //   NDEBUG → the dense 16-bit type_id<T>() counter (strict equality is meaningful);
    //   Debug  → typeid(T).name() (human-readable on mis-routing) → assert self-consistency instead.
#ifdef NDEBUG
    EXPECT_EQ(qb::Event::type_to_id<qb::KillEvent>(), qb::type_id<qb::KillEvent>());
    EXPECT_EQ(qb::Event::type_to_id<qb::SignalEvent>(), qb::type_id<qb::SignalEvent>());
#else
    const auto kill_a   = qb::Event::type_to_id<qb::KillEvent>();
    const auto kill_b   = qb::Event::type_to_id<qb::KillEvent>();
    const auto signal_a = qb::Event::type_to_id<qb::SignalEvent>();
    ASSERT_NE(kill_a, nullptr);
    ASSERT_NE(signal_a, nullptr);
    EXPECT_EQ(kill_a, kill_b) << "type_to_id<T>() must be stable across calls";
    EXPECT_STRNE(kill_a, signal_a) << "distinct event types must map to distinct ids";
#endif
}
