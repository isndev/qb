/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/stream-chunk-ring.cpp
 * @brief `qb::detail::chunk_ring<E>`, the stream's chunk buffer (Huly QB-215): FIFO order across
 *        wrap-around and growth, element lifetimes, slot alignment.
 *
 * The ring replaced a `std::deque<E>` whose MSVC block holds one element for any type wider than
 * 8 bytes (an allocation per chunk on Windows). Pure container arithmetic, no engine: unit tier.
 *   - ORDER: pushes and pops interleaved past the first capacity (8) keep FIFO order through the
 *     wrap-around and through a doubling, with the exact count at every step;
 *   - LIFETIMES: a counting element proves one live object per held chunk, none after `pop_front`,
 *     none after the ring is destroyed with chunks still inside, and that growth MOVES (no copies);
 *   - ALIGNMENT: a 64-byte-aligned element (the shape of a `qb::Event`) sits at a 64-byte-aligned
 *     address in every slot, before and after growth.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <cstdint>
#include <vector>

namespace {

struct Counted {
    static inline int live   = 0;
    static inline int copies = 0;
    static inline int moves  = 0;
    int               v;
    explicit Counted(int x)
        : v(x) {
        ++live;
    }
    Counted(const Counted &o)
        : v(o.v) {
        ++live;
        ++copies;
    }
    Counted(Counted &&o) noexcept
        : v(o.v) {
        ++live;
        ++moves;
    }
    ~Counted() {
        --live;
    }
};

struct alignas(64) Wide {
    std::uint64_t seq;
    char          pad[56];
};

} // namespace

TEST(ChunkRing, FifoOrderAcrossWrapAroundAndGrowth) {
    qb::detail::chunk_ring<int> r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
    int next_in = 0, next_out = 0;
    // 5 in, 3 out, 6 in: 8 held across the wrap-around of the first 8-slot ring.
    for (int i = 0; i < 5; ++i)
        r.emplace_back(int{next_in++});
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(r.front(), next_out++);
        r.pop_front();
    }
    for (int i = 0; i < 6; ++i)
        r.emplace_back(int{next_in++});
    EXPECT_EQ(r.size(), 8u);
    // One more forces the doubling with the head mid-way through the old ring.
    r.emplace_back(int{next_in++});
    EXPECT_EQ(r.size(), 9u);
    while (!r.empty()) {
        ASSERT_EQ(r.front(), next_out++);
        r.pop_front();
    }
    EXPECT_EQ(next_out, next_in) << "every pushed value came out once, in order";
    EXPECT_TRUE(r.empty());
    // Reuse after emptying: the storage is kept, order still holds over 100 more.
    for (int i = 0; i < 100; ++i)
        r.emplace_back(int{next_in++});
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(r.front(), next_out++);
        r.pop_front();
    }
}

TEST(ChunkRing, LifetimesAndMoveOnGrowth) {
    Counted::live = Counted::copies = Counted::moves = 0;
    {
        qb::detail::chunk_ring<Counted> r;
        for (int i = 0; i < 8; ++i)
            r.emplace_back(Counted{i}); // temporary moved in, then destroyed: live == held
        EXPECT_EQ(Counted::live, 8);
        EXPECT_EQ(Counted::copies, 0);
        const int moves_before = Counted::moves;
        r.emplace_back(Counted{8}); // doubling: the 8 held chunks are moved, never copied
        EXPECT_EQ(Counted::live, 9);
        EXPECT_EQ(Counted::copies, 0);
        EXPECT_GE(Counted::moves - moves_before, 8 + 1);
        r.pop_front();
        r.pop_front();
        EXPECT_EQ(Counted::live, 7) << "pop_front destroys what it drops";
        EXPECT_EQ(r.front().v, 2);
        r.clear();
        EXPECT_EQ(Counted::live, 0);
        for (int i = 0; i < 5; ++i)
            r.emplace_back(Counted{100 + i});
        EXPECT_EQ(Counted::live, 5);
    } // destroyed with 5 chunks inside
    EXPECT_EQ(Counted::live, 0) << "the ring destroys what it still holds";
    EXPECT_EQ(Counted::copies, 0);
}

TEST(ChunkRing, SlotsKeepTheElementAlignment) {
    qb::detail::chunk_ring<Wide> r;
    for (std::uint64_t i = 0; i < 40; ++i) { // 8 -> 16 -> 32 -> 64: three doublings
        r.emplace_back(Wide{i, {}});
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&r.front()) % 64, 0u);
    }
    for (std::uint64_t i = 0; i < 40; ++i) {
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(&r.front()) % 64, 0u) << "slot " << i;
        ASSERT_EQ(r.front().seq, i);
        r.pop_front();
    }
    EXPECT_TRUE(r.empty());
}
