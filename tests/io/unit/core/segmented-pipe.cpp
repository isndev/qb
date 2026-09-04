/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/segmented-pipe.cpp
 * @brief `qb::allocator::segmented_pipe<T>` — the event queue behind every `VirtualPipe`.
 *
 * `segmented_pipe` (qb/system/allocator/segmented_pipe.h) is what `Actor::push` allocates into and
 * what `VirtualCore::__receive__` / `__flush_all__` walk. It replaced `pipe<EventBucket>` for the
 * event queues: growth appends a segment instead of doubling-and-copying, consumed segments go
 * back to a per-owner pool so a sustained burst reuses cache-warm memory, and — the user-visible
 * half — an address handed out by `allocate_back` never moves. Pure in-memory data structure, NO
 * engine, so this is a strict `unit` test; the engine-level half of the same contract is
 * `PushReferenceStability.*` (tests/core/system/messaging/push-reference-stability.cpp).
 *
 * Every test ends with the pool reporting `outstanding() == 0`: a segment a pipe forgot to give
 * back is a leak the destructor of the pool cannot see.
 */

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/allocator/segmented_pipe.h>

namespace {

// Small segments so the chain grows in a handful of allocations: 16 items per segment, of which
// the header takes one on every platform (`segment_header` is three words, an item is 32 bytes).
struct Item {
    std::uint64_t value;
    std::uint64_t pad[3];
};
constexpr std::size_t kSegmentItems = 16;

using pool_t               = qb::allocator::segment_pool<Item, kSegmentItems>;
using pipe_t               = qb::allocator::segmented_pipe<Item, kSegmentItems>;
constexpr std::size_t kCap = pipe_t::segment_capacity;

// Append `n` items carrying `first, first+1, ...` and return the range's address.
Item *
push_run(pipe_t &p, std::size_t const n, std::uint64_t const first) {
    Item *const r = p.allocate_back(n);
    for (std::size_t i = 0; i < n; ++i)
        r[i].value = first + i;
    return r;
}

// Drain the pipe the way the engine does — segment by segment — collecting every value.
std::vector<std::uint64_t>
drain(pipe_t &p) {
    std::vector<std::uint64_t> out;
    while (!p.empty()) {
        auto const seg = p.front();
        EXPECT_FALSE(seg.empty()) << "front() is empty only when the pipe is";
        for (auto const &it : seg)
            out.push_back(it.value);
        p.pop_front();
    }
    return out;
}

} // namespace

// =============================================================================
// CONTRACT: growth never moves an item
// =============================================================================

/**
 * @test Addresses handed out by `allocate_back` stay valid and keep their contents across any
 *       number of later allocations, including every segment link.
 * @brief The contract `Actor::push` documents: a reference to a queued event is stable until the
 *        handler returns. `pipe<T>` broke it on every doubling; here every earlier pointer is
 *        dereferenced AFTER the chain has grown to many segments and still reads what was written.
 */
TEST(SegmentedPipeContract, GrowthKeepsEveryEarlierAddressValid) {
    pool_t pool;
    {
        pipe_t              p(pool);
        std::vector<Item *> ranges;
        for (std::uint64_t i = 0; i < 20 * kCap; ++i)
            ranges.push_back(push_run(p, 1, i));

        EXPECT_GE(p.segments(), 20u);
        for (std::uint64_t i = 0; i < ranges.size(); ++i)
            EXPECT_EQ(ranges[i]->value, i) << "item " << i << " moved or was overwritten by growth";

        // Writing through the first reference after the growth is well-defined, and is exactly
        // what a handler populating an event after further pushes does.
        ranges.front()->value = 0xFEED;
        auto const drained    = drain(p);
        ASSERT_EQ(drained.size(), 20 * kCap);
        EXPECT_EQ(drained.front(), 0xFEEDu);
        EXPECT_EQ(drained.back(), 20 * kCap - 1);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test A range never straddles two segments: the tail remainder is skipped, FIFO order is kept.
 * @brief `__flush_all__` builds runs from `front()`; a run may not cross a segment, and the walk
 *        relies on every allocation lying whole inside one `front()`.
 */
TEST(SegmentedPipeContract, RangesNeverStraddleSegments) {
    pool_t pool;
    {
        pipe_t p(pool);
        (void) push_run(p, kCap - 2, 0);        // leaves 2 free slots in the first segment
        Item *const wide = push_run(p, 5, 100); // cannot fit: goes whole into a new segment
        (void) push_run(p, 1, 200);

        auto const s0 = p.front();
        EXPECT_EQ(s0.size(), kCap - 2);
        EXPECT_TRUE(wide < s0.data() || wide >= s0.data() + kCap) << "the wide range must not start in the first segment";
        p.pop_front();
        auto const s1 = p.front();
        EXPECT_EQ(s1.data(), wide);
        EXPECT_EQ(s1.size(), 6u) << "5 wide + 1 after it, in the second segment";
        EXPECT_EQ(s1[5].value, 200u);
        p.pop_front();
        EXPECT_TRUE(p.empty());
        EXPECT_EQ(p.size(), 0u);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test `size()` counts live items across segments and `segments()` counts links, both tracking
 *       partial consumption.
 */
TEST(SegmentedPipeContract, SizeAndSegmentsTrackConsumption) {
    pool_t pool;
    {
        pipe_t p(pool);
        EXPECT_EQ(p.size(), 0u);
        EXPECT_EQ(p.segments(), 0u);
        EXPECT_TRUE(p.empty());
        EXPECT_TRUE(p.front().empty());

        (void) push_run(p, kCap, 0);
        (void) push_run(p, kCap, 0);
        (void) push_run(p, 3, 0);
        EXPECT_EQ(p.size(), 2 * kCap + 3);
        EXPECT_EQ(p.segments(), 3u);

        p.consume_front(4);
        EXPECT_EQ(p.size(), 2 * kCap - 1);
        EXPECT_EQ(p.front().size(), kCap - 4);
        p.consume_front(kCap - 4); // exhausts the head: auto-pop
        EXPECT_EQ(p.segments(), 2u);
        EXPECT_EQ(p.size(), kCap + 3);
        EXPECT_EQ(p.front().size(), kCap);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// POOL: reuse, retention, residency
// =============================================================================

/**
 * @test A popped segment is the one the next growth lands in (LIFO pool), and the working set of
 *       a produce/consume loop stays at two segments however long it runs.
 * @brief This is the cache argument for the design: a burst reuses the segment just read rather
 *        than fresh memory, so the memory system never enters the cell past the pool's high water.
 */
TEST(SegmentedPipePool, PoppedSegmentIsReusedFirst) {
    pool_t pool;
    {
        pipe_t                 p(pool);
        std::set<Item const *> bases;
        for (int round = 0; round < 1000; ++round) {
            (void) push_run(p, kCap, 0);
            (void) push_run(p, kCap, 0); // second segment
            bases.insert(p.front().data());
            p.pop_front(); // first segment back to the pool
            bases.insert(p.front().data());
            p.pop_front(); // pipe empty: tail rewound, stays resident
            EXPECT_TRUE(p.empty());
            EXPECT_EQ(p.segments(), 1u) << "one standard segment stays resident";
        }
        EXPECT_EQ(bases.size(), 2u) << "1000 rounds of two segments touched exactly two segments";
        EXPECT_EQ(pool.retained(), 1u);
        EXPECT_EQ(pool.outstanding(), 1u) << "the resident segment";
    }
    EXPECT_EQ(pool.outstanding(), 0u);
    EXPECT_EQ(pool.retained(), 2u) << "both segments retained for the next burst";
    pool.shrink();
    EXPECT_EQ(pool.retained(), 0u);
}

/**
 * @test The pool keeps a burst's segments at its high-water mark until `shrink()`; a second
 *       burst of the same size allocates nothing new.
 */
TEST(SegmentedPipePool, HighWaterRetainedAcrossBursts) {
    pool_t pool;
    {
        pipe_t p(pool);
        for (std::uint64_t i = 0; i < 50 * kCap; ++i)
            (void) push_run(p, 1, i);
        EXPECT_EQ(p.segments(), 50u);
        std::set<Item const *> first_burst;
        while (!p.empty()) {
            first_burst.insert(p.front().data());
            p.pop_front();
        }
        EXPECT_EQ(pool.retained(), 49u);
        EXPECT_EQ(pool.outstanding(), 1u);

        std::set<Item const *> second_burst;
        for (std::uint64_t i = 0; i < 50 * kCap; ++i)
            (void) push_run(p, 1, i);
        while (!p.empty()) {
            second_burst.insert(p.front().data());
            p.pop_front();
        }
        EXPECT_EQ(first_burst, second_burst) << "the second burst ran entirely in the first burst's segments";
        EXPECT_EQ(pool.retained(), 49u);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
    EXPECT_EQ(pool.retained(), 50u);
}

/**
 * @test `reset()` releases every segment but the tail, which is rewound and stays resident;
 *       `release_all()` gives the resident one back too.
 */
TEST(SegmentedPipePool, ResetKeepsOneResidentSegment) {
    pool_t pool;
    pipe_t p(pool);
    for (std::uint64_t i = 0; i < 5 * kCap; ++i)
        (void) push_run(p, 1, i);
    EXPECT_EQ(p.segments(), 5u);
    p.reset();
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.segments(), 1u);
    EXPECT_EQ(pool.retained(), 4u);
    EXPECT_EQ(p.front().size(), 0u);
    // The resident segment is written from its start again.
    Item *const r = push_run(p, 2, 7);
    EXPECT_EQ(p.front().data(), r);
    EXPECT_EQ(p.size(), 2u);
    p.release_all();
    EXPECT_EQ(p.segments(), 0u);
    EXPECT_EQ(pool.outstanding(), 0u);
    EXPECT_EQ(pool.retained(), 5u);
}

// =============================================================================
// OVERSIZE: dedicated segments
// =============================================================================

/**
 * @test A request wider than a standard segment gets a dedicated exactly-sized segment, which is
 *       freed — not pooled — once popped, and leaves the pipe holding nothing when it was the tail.
 * @brief `allocated_push` of a jumbo event: it must not double a pipe to 128 MB for one 65 MB
 *        event, and its memory must not be retained by a pool sized for 256 KB segments.
 */
TEST(SegmentedPipeOversize, DedicatedSegmentIsExactAndFreedOnPop) {
    pool_t pool;
    {
        pipe_t                p(pool);
        constexpr std::size_t jumbo = 10 * kCap + 3;
        (void) push_run(p, 2, 1);
        Item *const big = push_run(p, jumbo, 1000);
        (void) push_run(p, 1, 5000); // after a dedicated segment, a standard one is linked
        EXPECT_EQ(p.segments(), 3u);
        EXPECT_EQ(p.size(), jumbo + 3);

        p.pop_front(); // the two small items
        auto const s = p.front();
        EXPECT_EQ(s.data(), big);
        EXPECT_EQ(s.size(), jumbo) << "a dedicated segment holds exactly its request";
        EXPECT_EQ(s[jumbo - 1].value, 1000u + jumbo - 1);
        auto const alive_before = pool.outstanding();
        p.pop_front(); // dedicated: freed at once, never retained
        EXPECT_EQ(pool.outstanding(), alive_before - 1);
        EXPECT_EQ(pool.retained(), 1u) << "only the standard segment was pooled";
        EXPECT_EQ(p.front().size(), 1u);
        EXPECT_EQ(p.front()[0].value, 5000u);
        p.pop_front();
        EXPECT_TRUE(p.empty());
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test A dedicated segment that is also the tail is released on pop/reset rather than kept
 *       resident, and the next allocation starts a standard segment again.
 */
TEST(SegmentedPipeOversize, DedicatedTailIsNotKeptResident) {
    pool_t pool;
    pipe_t p(pool);
    (void) push_run(p, 3 * kCap, 0);
    EXPECT_EQ(p.segments(), 1u);
    p.pop_front();
    EXPECT_EQ(p.segments(), 0u) << "no dedicated segment stays resident";
    EXPECT_EQ(pool.outstanding(), 0u);
    (void) push_run(p, 1, 0);
    EXPECT_EQ(p.front().size(), 1u);
    p.reset();
    EXPECT_EQ(p.segments(), 1u);
    (void) push_run(p, 3 * kCap, 0); // oversize into an EMPTY resident tail: it is swapped out
    EXPECT_EQ(p.segments(), 1u) << "an empty resident segment is unlinked, not left in the chain";
    EXPECT_EQ(pool.retained(), 1u);
    EXPECT_EQ(p.front().size(), 3 * kCap);
    p.release_all();
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// free_back: the `send` shape
// =============================================================================

/**
 * @test `free_back(n)` gives back exactly the last `allocate_back(n)`, at a segment boundary too,
 *       and a fully emptied tail never lingers as an empty link in the chain.
 * @brief `VirtualCore::send` allocates, tries the peer's ring, and gives the range back when the
 *        ring took it. When that allocation was the first item of a freshly linked segment, the
 *        chain would otherwise carry a zero-item segment and `front()` could be empty on a
 *        non-empty pipe — the invariant every walk in the engine relies on.
 */
TEST(SegmentedPipeFreeBack, ExactAtBoundaryAndNoEmptyLinkLeftBehind) {
    pool_t pool;
    {
        pipe_t p(pool);
        (void) push_run(p, kCap, 0); // fills the first segment
        (void) p.allocate_back(4);   // links a second segment
        p.free_back(4);
        EXPECT_EQ(p.size(), kCap);
        EXPECT_EQ(p.segments(), 2u) << "the empty tail is still linked until something forces the slow path";

        // Consuming the first segment now makes the empty tail the head: `front()` may be empty
        // for a moment only when the pipe IS empty.
        p.pop_front();
        EXPECT_TRUE(p.empty());
        EXPECT_TRUE(p.front().empty());
        EXPECT_EQ(p.segments(), 1u);

        // Rebuild the shape and force the slow path on the empty tail with an oversize request:
        // the empty link is removed, not skipped over.
        (void) push_run(p, kCap, 0);
        (void) p.allocate_back(4);
        p.free_back(4);
        ASSERT_EQ(p.segments(), 2u);
        Item *const big = push_run(p, 2 * kCap, 9);
        EXPECT_EQ(p.segments(), 2u) << "first segment + dedicated; the empty one was unlinked";
        EXPECT_EQ(p.size(), 3 * kCap);
        p.pop_front();
        EXPECT_EQ(p.front().data(), big);
        EXPECT_FALSE(p.front().empty());
        p.pop_front();
        EXPECT_TRUE(p.empty());

        // Plain in-segment free_back.
        Item *const a = push_run(p, 3, 1);
        (void) p.allocate_back(2);
        p.free_back(2);
        Item *const b = push_run(p, 1, 4);
        EXPECT_EQ(b, a + 3) << "free_back handed the slots back exactly";
        EXPECT_EQ(p.size(), 4u);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// swap / move
// =============================================================================

/**
 * @test `swap` is an O(1) exchange of chains that keeps every address valid — the mono-pipe
 *       double-buffer in `VirtualCore::__receive__` swaps the self-core pipe every pass.
 */
TEST(SegmentedPipeOwnership, SwapExchangesChainsKeepingAddresses) {
    pool_t pool;
    {
        pipe_t      a(pool), b(pool);
        Item *const ra = push_run(a, kCap + 1, 10);
        Item *const rb = push_run(b, 2, 20);
        a.swap(b);
        EXPECT_EQ(a.size(), 2u);
        EXPECT_EQ(b.size(), kCap + 1);
        EXPECT_EQ(a.front().data(), rb);
        EXPECT_EQ(b.front().data(), ra);
        EXPECT_EQ(ra[kCap].value, 10u + kCap);
        auto const va = drain(a);
        auto const vb = drain(b);
        EXPECT_EQ(va, (std::vector<std::uint64_t>{20, 21}));
        EXPECT_EQ(vb.size(), kCap + 1);
        EXPECT_EQ(vb.back(), 10u + kCap);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test Move construction and move assignment transfer the chain, leave the source empty and
 *       segment-less, and release the target's previous chain on assignment.
 */
TEST(SegmentedPipeOwnership, MoveTransfersAndReleases) {
    pool_t pool;
    {
        pipe_t src(pool);
        (void) push_run(src, 3 * kCap, 0); // dedicated
        pipe_t dst(std::move(src));
        EXPECT_TRUE(src.empty());
        EXPECT_EQ(src.segments(), 0u);
        EXPECT_EQ(dst.size(), 3 * kCap);

        pipe_t other(pool);
        (void) push_run(other, 2, 5);
        auto const before = pool.outstanding();
        other             = std::move(dst); // other's standard segment goes back to the pool
        EXPECT_EQ(pool.outstanding(), before - 1);
        EXPECT_EQ(pool.retained(), 1u);
        EXPECT_EQ(other.size(), 3 * kCap);
        EXPECT_TRUE(dst.empty());
        EXPECT_EQ(dst.segments(), 0u);
        (void) push_run(src, 1, 0); // a moved-from pipe is usable again
        EXPECT_EQ(src.size(), 1u);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test `recycle_back` copies bytes into a fresh tail range whose address is stable.
 */
TEST(SegmentedPipeOwnership, RecycleBackCopiesIntoStableRange) {
    pool_t pool;
    {
        pipe_t p(pool);
        Item   src{42, {1, 2, 3}};
        Item  &copy = p.recycle_back(src, 1);
        src.value   = 0;
        for (std::uint64_t i = 0; i < 4 * kCap; ++i)
            (void) push_run(p, 1, i);
        EXPECT_EQ(copy.value, 42u);
        EXPECT_EQ(copy.pad[2], 3u);
        EXPECT_EQ(p.front().data(), &copy);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// Default geometry: what the engine instantiates
// =============================================================================

/**
 * @test The default segment is 256 KB of items with a one-item header, and the header fits the
 *       allocation's alignment — the geometry `VirtualPipe` gets.
 */
TEST(SegmentedPipeGeometry, DefaultSegmentIs256KB) {
    using engine_pipe           = qb::allocator::segmented_pipe<Item>;
    constexpr std::size_t items = (256u * 1024u) / sizeof(Item);
    EXPECT_EQ(engine_pipe::segment_capacity, items - 1);
    engine_pipe::pool_type pool;
    {
        engine_pipe p(pool);
        Item *const r = p.allocate_back(engine_pipe::segment_capacity);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r) % alignof(Item), 0u);
        EXPECT_EQ(p.segments(), 1u);
        (void) p.allocate_back(1);
        EXPECT_EQ(p.segments(), 2u) << "one item past a full segment links the next";
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}
