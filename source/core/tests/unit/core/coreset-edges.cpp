/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/coreset-edges.cpp
 * @brief `qb::CoreSet` — the engine-internal CoreId->dense-index mapping (`qb/core/CoreSet.h`,
 *        `src/CoreSet.cpp`). Pure value logic: NO `qb::Main`, NO VirtualCore, NO event loop.
 *
 * CoreSet wraps a `qb::CoreIdSet` and pre-computes an O(1) resolve table, but its own accessors
 * (`getSize()` / `getNbCore()` / `raw()` / `resolve()`) were never exercised on their own — they
 * only ran incidentally when a real engine started. These cases pin the accessor contract directly
 * from a hand-built set, with an INDEPENDENT oracle (the set membership we constructed), and nail
 * the two contracts a reader is most likely to confuse:
 *
 *   - `getNbCore()` is the *count* of member core ids (`CoreIdSet::size()`), while `getSize()` is
 *     the *highest member id + 1* (the dense-table span), NOT the member count and NOT `MaxCores` —
 *     a non-contiguous set makes the two diverge (`{0,5}` -> nbCore 2, size 6), which is asserted.
 *   - `resolve(id)` returns the ascending 0-based rank of a member (iteration is bit-ascending), and
 *     an out-of-range id (>= MaxCores) resolves to 0 via the noexcept bounds-check rather than
 *     throwing from the fixed `std::array<uint8_t, MaxCores>` (the misaddressed-event guard).
 *
 * The empty-set and `build(n)` factory paths are the two constructions the engine actually uses.
 */

#include <gtest/gtest.h>

#include <qb/core/ActorId.h> // qb::CoreIdSet
#include <qb/core/CoreSet.h>

using qb::CoreId;
using qb::CoreIdSet;
using qb::CoreSet;

// ---------------------------------------------------------------------------
// Empty set: a CoreSet built from an empty CoreIdSet has zero members and a
// zero dense span (no bit set -> the _size scan returns 0).
// ---------------------------------------------------------------------------

TEST(CoreSetEdges, EmptySetHasZeroSizeAndNbCore) {
    CoreSet cs{CoreIdSet{}};
    EXPECT_EQ(cs.getSize(), 0u) << "no member -> the dense-table span is 0";
    EXPECT_EQ(cs.getNbCore(), 0u) << "no member -> zero cores";
    EXPECT_TRUE(cs.raw().empty());
    // resolve() on an empty set is well-defined: every id is a non-member, and an out-of-range id
    // is bounds-checked to 0 rather than throwing from the fixed-size array (noexcept guard).
    EXPECT_EQ(cs.resolve(0), static_cast<CoreId>(0));
    EXPECT_EQ(cs.resolve(300), static_cast<CoreId>(0)) << "id >= MaxCores must resolve to 0, never throw";
}

// ---------------------------------------------------------------------------
// build(n): the common factory the engine uses — {0, 1, ..., n-1}. nbCore is
// the count (n); size is the highest id + 1, which for a contiguous 0-based
// set is also n. resolve maps each member to its own ascending rank.
// ---------------------------------------------------------------------------

TEST(CoreSetEdges, BuildFourIsContiguousZeroBased) {
    const CoreSet cs = CoreSet::build(4);
    EXPECT_EQ(cs.getNbCore(), 4u);
    EXPECT_EQ(cs.getSize(), 4u) << "highest member id (3) + 1 == 4 for a contiguous {0,1,2,3}";

    // raw() is the {0,1,2,3} set we asked for.
    const CoreIdSet &raw = cs.raw();
    EXPECT_EQ(raw.size(), 4u);
    for (CoreId c = 0; c < 4; ++c)
        EXPECT_TRUE(raw.contains(c)) << "core " << c << " must be a member";
    EXPECT_FALSE(raw.contains(static_cast<CoreId>(4)));

    // Iteration is bit-ascending, so each member's dense rank equals its own value.
    EXPECT_EQ(cs.resolve(0), static_cast<CoreId>(0));
    EXPECT_EQ(cs.resolve(1), static_cast<CoreId>(1));
    EXPECT_EQ(cs.resolve(3), static_cast<CoreId>(3));
    // Out-of-range id is bounds-checked to 0 (the misaddressed-event guard), not a throw.
    EXPECT_EQ(cs.resolve(300), static_cast<CoreId>(0));
}

// ---------------------------------------------------------------------------
// getSize() vs getNbCore() diverge on a NON-contiguous set: {0, 5} has 2
// members (nbCore) but a dense span of 6 (highest id 5 + 1). This is the pair
// most likely to be confused — pin the exact contract, and the ascending rank
// mapping resolve() derives from bit-ascending iteration.
// ---------------------------------------------------------------------------

TEST(CoreSetEdges, NonContiguousSetSeparatesSizeFromNbCore) {
    CoreSet cs{CoreIdSet{0, 5}};
    EXPECT_EQ(cs.getNbCore(), 2u) << "getNbCore() is the member COUNT";
    EXPECT_EQ(cs.getSize(), 6u) << "getSize() is highest member id (5) + 1, NOT the count and NOT MaxCores";

    // Ranks are assigned in bit-ascending order: 0 -> rank 0, 5 -> rank 1.
    EXPECT_EQ(cs.resolve(0), static_cast<CoreId>(0));
    EXPECT_EQ(cs.resolve(5), static_cast<CoreId>(1));

    EXPECT_EQ(cs.raw().size(), 2u);
    EXPECT_TRUE(cs.raw().contains(static_cast<CoreId>(0)));
    EXPECT_TRUE(cs.raw().contains(static_cast<CoreId>(5)));
    EXPECT_FALSE(cs.raw().contains(static_cast<CoreId>(1)));
}
