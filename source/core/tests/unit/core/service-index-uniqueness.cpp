/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/service-index-uniqueness.cpp
 * @brief `ServiceActor<Tag>::ServiceIndex` uniqueness + concurrent-first-access stability.
 *
 * A service actor is identified by a per-Tag `ServiceId` assigned through a magic-static index
 * (`Actor::registerIndex<Tag>()`). The id must be unique per tag, identical for every reader of
 * the same tag (even when many threads race the first access), and 1-based (0 marks "not a
 * service"). Pure logic exercised via the public `Actor::getServiceId<Tag>(CoreId)`.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>

namespace {
struct ServiceTagA {};
struct ServiceTagB {};
struct ServiceTagC {};
} // namespace

TEST(ServiceIndex, IsUniqueAcrossTagsAndStableUnderConcurrency) {
    constexpr std::size_t kThreads = 8;
    constexpr qb::CoreId  kCore    = 0;
    std::atomic<bool>     go{false};
    // getServiceId<Tag>(core).sid() yields the per-tag ServiceId we compare.
    std::array<qb::ServiceId, kThreads> a{}, b{}, c{};
    std::vector<std::thread>            workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &go, &a, &b, &c] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            a[i] = qb::Actor::getServiceId<ServiceTagA>(kCore).sid();
            b[i] = qb::Actor::getServiceId<ServiceTagB>(kCore).sid();
            c[i] = qb::Actor::getServiceId<ServiceTagC>(kCore).sid();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : workers)
        t.join();

    // Within one tag, every reader observes the same id.
    for (std::size_t i = 1; i < kThreads; ++i) {
        EXPECT_EQ(a[i], a[0]);
        EXPECT_EQ(b[i], b[0]);
        EXPECT_EQ(c[i], c[0]);
    }
    // Across tags, ids differ — they identify distinct services.
    EXPECT_NE(a[0], b[0]);
    EXPECT_NE(b[0], c[0]);
    EXPECT_NE(a[0], c[0]);
    // 1-based; 0 marks "non-service".
    EXPECT_GT(a[0], qb::ServiceId{0});
    EXPECT_GT(b[0], qb::ServiceId{0});
    EXPECT_GT(c[0], qb::ServiceId{0});
}

TEST(ServiceIndex, SameTagSameIdAcrossCores) {
    // The ServiceIndex (the per-Tag component) is core-independent: the same tag yields the same
    // ServiceId regardless of which core it is requested for (the CoreId lives in the ActorId's
    // index, not in the ServiceId). Asserting this pins the "index is per-tag, not per-core" rule.
    const auto core0 = qb::Actor::getServiceId<ServiceTagA>(0).sid();
    const auto core1 = qb::Actor::getServiceId<ServiceTagA>(1).sid();
    const auto core2 = qb::Actor::getServiceId<ServiceTagA>(2).sid();
    EXPECT_EQ(core0, core1);
    EXPECT_EQ(core1, core2);
    EXPECT_GT(core0, qb::ServiceId{0});
}
