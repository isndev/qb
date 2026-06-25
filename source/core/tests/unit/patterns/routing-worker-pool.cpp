/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/routing-worker-pool.cpp
 * @brief Pure-logic contract of `qb::WorkerPool` (the round-robin / sticky-by-key router).
 *
 * `WorkerPool` is an allocation-light value type: a list of worker `ActorId`s plus a
 * round-robin cursor. It owns no engine, touches no event loop, and is therefore tested
 * here at UNIT tier with placeholder `ActorId`s and zero `qb::Main` — every case is
 * deterministic and parallel-safe.
 *
 * What is proven (the full public contract of qb/core/patterns/routing.h):
 *   - `next()`  cycles strictly through the pool and wraps back to the head (round-robin);
 *   - `for_key(k)` is a stable hash: `k % size()` -> same worker for the same key, and a
 *     uniform spread of distinct keys across the pool;
 *   - `add`/`remove`/`size`/`empty` resize the pool, and `remove` re-clamps the cursor so
 *     a subsequent `next()` can never index out of bounds (the wrap-after-shrink invariant);
 *   - removing from an empty pool is a safe no-op; the pool is reusable after refill;
 *   - `for_key` re-maps after a `remove` shrinks the modulus (the missing case the source
 *     never covered) — the sticky mapping is only stable *until the pool size changes*.
 *
 * No engine, no globals, no timing: these assertions are framework-truth on a value type.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <qb/core/ActorId.h>
#include <qb/core/patterns/routing.h>

using qb::ActorId;
using qb::WorkerPool;

// ===========================================================================
// next() — round-robin cycling.
// ===========================================================================
TEST(WorkerPoolUnit, RoundRobinCyclesAndWraps) {
    ActorId    a(1), b(2), c(3);
    WorkerPool r{{a, b, c}};
    EXPECT_EQ(r.size(), 3u);
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(r.workers().size(), 3u); // workers() exposes the backing list verbatim

    EXPECT_EQ(r.next(), a);
    EXPECT_EQ(r.next(), b);
    EXPECT_EQ(r.next(), c);
    EXPECT_EQ(r.next(), a); // wraps back to the head after a full cycle
    EXPECT_EQ(r.next(), b);

    // A second full lap reproduces the identical sequence (cursor is the only state).
    EXPECT_EQ(r.next(), c);
    EXPECT_EQ(r.next(), a);
    EXPECT_EQ(r.next(), b);
    EXPECT_EQ(r.next(), c);
}

// ===========================================================================
// for_key() — sticky-by-key, deterministic, and well-spread.
// ===========================================================================
TEST(WorkerPoolUnit, ForKeyIsStickyAndDistributes) {
    ActorId    w0(10), w1(20), w2(30);
    WorkerPool r{{w0, w1, w2}};

    // Exact modulus mapping, not just "some worker".
    EXPECT_EQ(r.for_key(0), w0);
    EXPECT_EQ(r.for_key(1), w1);
    EXPECT_EQ(r.for_key(2), w2);
    EXPECT_EQ(r.for_key(3), w0); // 3 % 3 == 0 -> wraps to the head
    EXPECT_EQ(r.for_key(4), w1);
    EXPECT_EQ(r.for_key(5), w2);

    // Deterministic for the same key, and const (does not advance the round-robin cursor).
    EXPECT_EQ(r.for_key(99), r.for_key(99));
    EXPECT_EQ(r.for_key(99), r.workers()[99u % 3u]);
    EXPECT_EQ(r.next(), w0) << "for_key() must not perturb the next() cursor";

    // A run of distinct keys lands on every worker (uniform spread, no starvation).
    int hits[3] = {0, 0, 0};
    for (std::uint64_t k = 0; k < 30; ++k) {
        const auto picked = r.for_key(k);
        if (picked == w0)
            ++hits[0];
        else if (picked == w1)
            ++hits[1];
        else if (picked == w2)
            ++hits[2];
        else
            ADD_FAILURE() << "for_key returned an id outside the pool";
    }
    EXPECT_EQ(hits[0], 10);
    EXPECT_EQ(hits[1], 10);
    EXPECT_EQ(hits[2], 10);
}

// ===========================================================================
// add / remove / size / empty — resize the pool.
// ===========================================================================
TEST(WorkerPoolUnit, AddRemoveResizesPool) {
    WorkerPool r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);

    ActorId a(1), b(2);
    r.add(a);
    r.add(b);
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 2u);

    r.remove(a);
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.workers().front(), b);
    EXPECT_EQ(r.next(), b);
    EXPECT_EQ(r.next(), b); // a single worker -> always itself
}

// ===========================================================================
// remove() — wrap the cursor after a shrink; empty-edge safety; reuse after refill.
// ===========================================================================
TEST(WorkerPoolUnit, RemoveWrapsCursorAndHandlesEmptyEdges) {
    ActorId    a(1), b(2), c(3);
    WorkerPool r{{a, b, c}};
    (void) r.next(); // cursor -> 1
    (void) r.next(); // cursor -> 2 (points at the last slot)
    r.remove(c);     // size now 2; cursor (2) is out of range and must be re-clamped

    EXPECT_EQ(r.size(), 2u);
    const auto n = r.next(); // must index a valid remaining worker, never out of bounds, never c
    EXPECT_TRUE(n == a || n == b);
    EXPECT_NE(n, c);

    // Drain to empty.
    r.remove(a);
    r.remove(b);
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);

    // Removing from an empty pool is a safe no-op (does not throw, does not corrupt state).
    r.remove(a);
    EXPECT_TRUE(r.empty());

    // Reusable after a refill.
    r.add(c);
    EXPECT_FALSE(r.empty());
    EXPECT_EQ(r.next(), c);
    EXPECT_EQ(r.next(), c);
}

// ===========================================================================
// for_key() after remove() — the sticky mapping re-hashes when the pool shrinks.
// (Missing case the source never exercised: stickiness holds only *until* size changes.)
// ===========================================================================
TEST(WorkerPoolUnit, ForKeyRemapsAfterRemoveShrinksModulus) {
    ActorId    w0(10), w1(20), w2(30);
    WorkerPool r{{w0, w1, w2}};

    // Key 7: 7 % 3 == 1 -> w1, before the pool shrinks.
    EXPECT_EQ(r.for_key(7), w1);

    r.remove(w0); // pool now {w1, w2}; the modulus drops from 3 to 2
    EXPECT_EQ(r.size(), 2u);

    // Same key 7 now re-hashes against the new size: 7 % 2 == 1 -> the 2nd remaining worker (w2).
    EXPECT_EQ(r.for_key(7), w2);
    EXPECT_EQ(r.for_key(0), w1); // 0 % 2 == 0 -> head of the shrunken pool
    EXPECT_EQ(r.for_key(1), w2);
    EXPECT_EQ(r.for_key(2), w1); // wraps

    // Still deterministic for repeated lookups against the new size.
    EXPECT_EQ(r.for_key(7), r.for_key(7));
    EXPECT_EQ(r.for_key(7), r.workers()[7u % 2u]);
}
