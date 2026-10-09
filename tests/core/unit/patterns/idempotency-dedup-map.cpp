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
 *   - copies own their index and remain independent through source mutation/destruction;
 *   - failed copy assignment leaves the destination unchanged; moves keep a valid LRU.
 *   - a hash with throwing swap keeps copy construction but cannot use copy assignment.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace throwing_swap_test {
struct Key {
    int  value;
    bool operator==(const Key &) const = default;
};
} // namespace throwing_swap_test

template <>
struct std::hash<throwing_swap_test::Key> {
    std::size_t
    operator()(throwing_swap_test::Key key) const noexcept {
        return std::hash<int>{}(key.value);
    }
    friend void
    swap(hash &, hash &) noexcept(false) {}
};

static_assert(!std::is_nothrow_swappable_v<std::hash<throwing_swap_test::Key>>);
static_assert(!std::is_copy_assignable_v<qb::dedup_map<throwing_swap_test::Key, int>>);
static_assert(std::is_copy_assignable_v<qb::dedup_map<int, int>>);

namespace {
struct CopyFault {
    int   value;
    bool *fail_copy;

    CopyFault(int value, bool &fail_copy)
        : value(value)
        , fail_copy(&fail_copy) {}
    CopyFault(const CopyFault &other)
        : value(other.value)
        , fail_copy(other.fail_copy) {
        if (*fail_copy)
            throw std::runtime_error("copy failed");
    }
    CopyFault(CopyFault &&)                 = default;
    CopyFault &operator=(const CopyFault &) = default;
    CopyFault &operator=(CopyFault &&)      = default;
};
} // namespace

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
    EXPECT_TRUE(m.contains(1)); // peek at 1 — must NOT promote it
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

TEST(DedupMap, CopyConstructionKeepsValuesAndLruIndependent) {
    qb::dedup_map<int, int> source(2);
    source.put(1, 10);
    source.put(2, 20);

    qb::dedup_map<int, int> copy(source);
    ASSERT_NE(copy.find(1), nullptr); // promote 1 only in the copy
    EXPECT_EQ(*copy.find(1), 10);
    source.put(1, 11); // changing the source must not change the copy
    EXPECT_EQ(*copy.find(1), 10);

    copy.put(3, 30); // copy's LRU is 2
    EXPECT_FALSE(copy.contains(2));
    EXPECT_TRUE(copy.contains(1));
    EXPECT_TRUE(source.contains(2));
    EXPECT_EQ(*source.find(1), 11);
}

TEST(DedupMap, CopyConstructionAllowsHashWithThrowingSwap) {
    qb::dedup_map<throwing_swap_test::Key, int> source(2);
    source.put({1}, 10);
    qb::dedup_map<throwing_swap_test::Key, int> copy(source);
    ASSERT_NE(copy.find({1}), nullptr);
    EXPECT_EQ(*copy.find({1}), 10);
}

TEST(DedupMap, CopyAssignmentSurvivesSourceDestruction) {
    qb::dedup_map<int, int> copy(2);
    copy.put(9, 90);
    {
        qb::dedup_map<int, int> source(2);
        source.put(1, 10);
        source.put(2, 20);
        copy = source;
    }

    EXPECT_FALSE(copy.contains(9));
    ASSERT_NE(copy.find(1), nullptr);
    EXPECT_EQ(*copy.find(1), 10);
    copy.put(3, 30);
    EXPECT_FALSE(copy.contains(2));
    EXPECT_TRUE(copy.contains(1));
    const auto &alias = copy;
    copy              = alias;
    EXPECT_EQ(*copy.find(1), 10);
}

TEST(DedupMap, FailedCopyAssignmentLeavesDestinationUnchanged) {
    bool                          fail_copy = false;
    qb::dedup_map<int, CopyFault> source(2);
    source.put(1, CopyFault{10, fail_copy});
    qb::dedup_map<int, CopyFault> destination(1);
    destination.put(9, CopyFault{90, fail_copy});

    fail_copy = true;
    EXPECT_THROW(destination = source, std::runtime_error);
    fail_copy = false;
    EXPECT_EQ(destination.capacity(), 1u);
    EXPECT_TRUE(destination.contains(9));
    EXPECT_FALSE(destination.contains(1));
    ASSERT_NE(destination.find(9), nullptr);
    EXPECT_EQ(destination.find(9)->value, 90);
}

TEST(DedupMap, MoveConstructionAndAssignmentRetainLru) {
    qb::dedup_map<int, int> source(2);
    source.put(1, 10);
    source.put(2, 20);
    qb::dedup_map<int, int> moved(std::move(source));
    source.clear();
    ASSERT_NE(moved.find(1), nullptr);
    moved.put(3, 30);
    EXPECT_FALSE(moved.contains(2));

    qb::dedup_map<int, int> assigned(1);
    assigned.put(9, 90);
    assigned = std::move(moved);
    moved.clear();
    EXPECT_EQ(assigned.capacity(), 2u);
    ASSERT_NE(assigned.find(1), nullptr);
    EXPECT_EQ(*assigned.find(1), 10);
    EXPECT_TRUE(assigned.contains(3));
}
