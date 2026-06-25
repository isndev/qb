/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/actor-id.cpp
 * @brief `qb::ActorId` / `qb::BroadcastId` / `qb::CoreIdBitSet` value types (`qb/core/ActorId.h`).
 *
 * Pure bit-packing and bitset logic: NO actor is ever instantiated, NO `qb::Main`, NO event loop.
 * Round-trip and packing assertions check *derived* structure (`sid()` / `index()` recovered from
 * the packed `uint32`), not the raw value the test set, so they verify the codec rather than echo
 * input. `CoreIdBitSet` cases pin the out-of-range filter (>= MaxCores silently ignored) and the
 * exact membership including the boundary value.
 *
 * Added over the original: NON-broadcast `(sid, index)` construction (every prior case routed
 * through `BroadcastId`, so the normal-service packing path — sid != BroadcastSid — was untested;
 * exercised here via a test-only subclass that exposes the protected two-arg ctor the same way
 * `BroadcastId` does); the exact 255-valid / 256-ignored `CoreIdBitSet` boundary (the original only
 * probed 300); and a set-from-raw round-trip equality (`CoreIdBitSet(b.raw()) == b` by membership).
 */

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>
#include <qb/core/ActorId.h>

using qb::ActorId;
using qb::BroadcastId;
using qb::CoreId;
using qb::CoreIdBitSet;
using qb::ServiceId;

namespace {
// Test-only handle that exposes ActorId's protected (ServiceId, CoreId) constructor — the
// normal-service packing path. Mirrors how qb::BroadcastId (a public subclass) reaches the same
// protected ctor with BroadcastSid; here we drive it with an arbitrary non-broadcast sid.
struct NormalId : public ActorId {
    NormalId(ServiceId sid, CoreId index) noexcept
        : ActorId(sid, index) {}
};
} // namespace

// --- ActorId -----------------------------------------------------------------

TEST(ActorId, DefaultIsNotFoundAndInvalid) {
    ActorId id;
    EXPECT_FALSE(id.is_valid());
    EXPECT_EQ(static_cast<std::uint32_t>(id), ActorId::NotFound);
    EXPECT_FALSE(id.is_broadcast());
}

TEST(ActorId, Uint32RoundTripPreservesSidAndIndex) {
    BroadcastId   b(7); // sid = BroadcastSid, index = 7
    std::uint32_t packed = static_cast<std::uint32_t>(b);
    ActorId       round(packed); // unpack
    EXPECT_EQ(static_cast<std::uint32_t>(round), packed);
    EXPECT_EQ(round.sid(), b.sid());
    EXPECT_EQ(round.index(), b.index());
}

TEST(ActorId, BroadcastIdProperties) {
    BroadcastId b(3);
    EXPECT_TRUE(b.is_broadcast());
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.sid(), ActorId::BroadcastSid);
    EXPECT_EQ(b.index(), static_cast<CoreId>(3));
}

TEST(ActorId, EqualityViaUint32) {
    BroadcastId b(5);
    ActorId     a(static_cast<std::uint32_t>(b));
    EXPECT_EQ(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    ActorId none;
    EXPECT_NE(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(none));
}

TEST(ActorId, NonBroadcastSidIndexConstruction) {
    // The normal-service path: a non-broadcast sid packs with a core index and survives the
    // uint32 round-trip with both components recoverable. (Every other case routes through
    // BroadcastId, whose sid is always BroadcastSid — this exercises sid != BroadcastSid.)
    constexpr ServiceId sid   = 5;
    constexpr CoreId    index = 2;
    NormalId            id(sid, index);

    EXPECT_EQ(id.sid(), sid);
    EXPECT_EQ(id.index(), index);
    EXPECT_FALSE(id.is_broadcast()) << "a sid below BroadcastSid is NOT a broadcast id";
    EXPECT_TRUE(id.is_valid()) << "a non-zero packed id is valid";

    // uint32 codec is a faithful inverse: unpacking the packed value recovers both components.
    const std::uint32_t packed = static_cast<std::uint32_t>(id);
    ActorId             round(packed);
    EXPECT_EQ(round.sid(), sid);
    EXPECT_EQ(round.index(), index);
    EXPECT_EQ(static_cast<std::uint32_t>(round), packed);
    EXPECT_FALSE(round.is_broadcast());

    // A normal id with the same components but a different core is a distinct value.
    NormalId other(sid, static_cast<CoreId>(index + 1));
    EXPECT_NE(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(other));

    // Boundary: sid == BroadcastSid IS broadcast even via this construction path.
    NormalId asBroadcast(ActorId::BroadcastSid, index);
    EXPECT_TRUE(asBroadcast.is_broadcast());

    // The all-zero packing is exactly NotFound / invalid.
    NormalId zero(0, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(zero), ActorId::NotFound);
    EXPECT_FALSE(zero.is_valid());
}

// --- CoreIdBitSet ------------------------------------------------------------

TEST(CoreIdBitSet, DefaultEmpty) {
    CoreIdBitSet s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.contains(0));
}

TEST(CoreIdBitSet, InsertContainsRemoveClear) {
    CoreIdBitSet s;
    s.insert(1);
    s.emplace(4); // alias for insert
    EXPECT_TRUE(s.contains(1));
    EXPECT_TRUE(s.contains(4));
    EXPECT_FALSE(s.contains(2));
    EXPECT_EQ(s.size(), 2u);
    EXPECT_FALSE(s.empty());
    s.remove(1);
    EXPECT_FALSE(s.contains(1));
    EXPECT_EQ(s.size(), 1u);
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
}

TEST(CoreIdBitSet, OutOfRangeIdsAreIgnored) {
    CoreIdBitSet s;
    s.insert(static_cast<CoreId>(300)); // >= MaxCores (256) → ignored
    EXPECT_TRUE(s.empty());
    EXPECT_FALSE(s.contains(static_cast<CoreId>(300)));
    s.remove(static_cast<CoreId>(300)); // no-op, must not crash
}

TEST(CoreIdBitSet, ExactMaxCoresBoundary) {
    // MaxCores == 256, so valid indices are [0, 255]. Pin the exact boundary: 255 is accepted,
    // 256 (the first out-of-range value) is silently dropped — the original only probed 300.
    static_assert(qb::MaxCores == 256, "boundary cases assume a 256-core ceiling");
    CoreIdBitSet s;

    s.insert(static_cast<CoreId>(255)); // last valid index
    EXPECT_TRUE(s.contains(static_cast<CoreId>(255)));
    EXPECT_EQ(s.size(), 1u);

    s.insert(static_cast<CoreId>(256)); // first out-of-range index → ignored
    EXPECT_FALSE(s.contains(static_cast<CoreId>(256)));
    EXPECT_EQ(s.size(), 1u) << "256 must not be stored";

    // The initializer-list ctor applies the same filter: 256 is dropped, 255 kept.
    CoreIdBitSet viaList{static_cast<CoreId>(255), static_cast<CoreId>(256)};
    EXPECT_EQ(viaList.size(), 1u);
    EXPECT_TRUE(viaList.contains(static_cast<CoreId>(255)));
    EXPECT_FALSE(viaList.contains(static_cast<CoreId>(256)));

    // contains() is also range-guarded: querying an out-of-range id is false, never a crash.
    EXPECT_FALSE(s.contains(static_cast<CoreId>(256)));
    EXPECT_FALSE(s.contains(std::numeric_limits<CoreId>::max()));
}

TEST(CoreIdBitSet, SetFromRawRoundTripEquality) {
    // raw() (an unordered_set<CoreId>) reconstructs an equal bitset via the set ctor: round-trip
    // membership must be identical, including the high boundary index 255.
    CoreIdBitSet b{0, 4, 200, static_cast<CoreId>(255)};
    CoreIdBitSet rebuilt(b.raw());

    EXPECT_EQ(rebuilt.size(), b.size());
    EXPECT_EQ(rebuilt.raw(), b.raw()) << "set-from-raw must reproduce the exact membership";
    EXPECT_EQ(rebuilt.bits(), b.bits());
    for (CoreId c : b)
        EXPECT_TRUE(rebuilt.contains(c)) << "member " << c << " lost in raw round-trip";
    EXPECT_TRUE(rebuilt.contains(static_cast<CoreId>(255)));
}

TEST(CoreIdBitSet, InitializerListAndSetConstructors) {
    CoreIdBitSet a{2, 5, 9};
    EXPECT_EQ(a.size(), 3u);
    EXPECT_TRUE(a.contains(5));

    qb::unordered_set<CoreId> src{3, 7};
    CoreIdBitSet              b(src);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_TRUE(b.contains(3));
    EXPECT_TRUE(b.contains(7));
}

TEST(CoreIdBitSet, ConvertersAndRawBits) {
    CoreIdBitSet s{1, 4, 8};
    auto         v = s.to_vector();
    EXPECT_EQ(v.size(), 3u);
    EXPECT_TRUE(std::find(v.begin(), v.end(), 4) != v.end());

    auto us = s.to_unordered_set();
    EXPECT_EQ(us.size(), 3u);
    EXPECT_EQ(us, s.raw());

    EXPECT_EQ(s.bits().count(), 3u); // underlying std::bitset
}

TEST(CoreIdBitSet, Iteration) {
    CoreIdBitSet        s{0, 3, 200};
    std::vector<CoreId> seen;
    for (CoreId c : s)
        seen.push_back(c);
    std::sort(seen.begin(), seen.end());
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], static_cast<CoreId>(0));
    EXPECT_EQ(seen[1], static_cast<CoreId>(3));
    EXPECT_EQ(seen[2], static_cast<CoreId>(200));
}
