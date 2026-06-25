/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/idempotency-dedup-map.cpp
 * @brief LRU semantics of `qb::dedup_map` — the store behind `answer_idempotent` (pure, no engine).
 *
 * `dedup_map` is a bounded LRU cache of `key → response` with NO threading and NO engine, so its
 * order/eviction logic is fully unit-testable (daemon-free, parallel-safe). The behaviour the
 * idempotent responder relies on is pinned exactly:
 *   - `find` HITS return the stored value AND promote the key to most-recently-used;
 *   - inserting past capacity evicts the LEAST-recently-used entry (and a prior `find` changes which
 *     entry that is — the canonical LRU differential);
 *   - `put` on an existing key UPDATES in place (no growth, value replaced) and promotes;
 *   - `contains` reports membership WITHOUT promoting (a peek must not perturb LRU order);
 *   - `clear` empties the map; `capacity` clamps to >= 1.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>

TEST(DedupMap, FindPromotesAndEvictsLeastRecentlyUsed) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    m.put(2, 20);
    EXPECT_EQ(m.size(), 2u);

    // Touch 1 → 1 becomes MRU, so 2 is now the LRU candidate for eviction.
    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 10);

    m.put(3, 30); // over capacity → evict the LRU (2), NOT the just-touched 1.
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.find(2), nullptr) << "the least-recently-used entry must be evicted";
    ASSERT_NE(m.find(1), nullptr) << "a find()-promoted entry must survive eviction";
    ASSERT_NE(m.find(3), nullptr);
    EXPECT_EQ(*m.find(3), 30);
}

TEST(DedupMap, PutUpdatesInPlaceWithoutGrowth) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    m.put(1, 11); // same key → replace value, no new entry
    EXPECT_EQ(m.size(), 1u) << "re-putting a key must update in place, not grow the map";
    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 11);
}

TEST(DedupMap, ContainsDoesNotPromote) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    m.put(2, 20);
    EXPECT_TRUE(m.contains(1));  // peek at 1 — must NOT promote it
    EXPECT_FALSE(m.contains(9));
    // Since contains() did not promote 1, the LRU is still 1; inserting 3 evicts 1, not 2.
    m.put(3, 30);
    EXPECT_FALSE(m.contains(1)) << "contains() must not promote — 1 stays LRU and is evicted";
    EXPECT_TRUE(m.contains(2));
    EXPECT_TRUE(m.contains(3));
}

TEST(DedupMap, ClearEmpties) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    m.put(2, 20);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.contains(1));
    EXPECT_FALSE(m.contains(2));
    EXPECT_EQ(m.find(1), nullptr);
}

TEST(DedupMap, ZeroCapacityClampsToOne) {
    qb::dedup_map<int, int> z(0); // capacity clamps to >= 1
    EXPECT_EQ(z.capacity(), 1u);
    z.put(1, 1);
    z.put(2, 2); // evicts 1 (cap 1)
    EXPECT_EQ(z.size(), 1u);
    EXPECT_FALSE(z.contains(1));
    EXPECT_TRUE(z.contains(2));
}
