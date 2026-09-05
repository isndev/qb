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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/allocator/segmented_pipe.h>
#include <qb/system/allocator/slab.h>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

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
 * @test The default segment is 256 KB of items with a one-item header and a 64-slot stagger, and
 *       the header fits the allocation's alignment — the geometry `VirtualPipe` gets. The
 *       guaranteed capacity is the raw one minus the widest stagger (63 lines): with 32-byte
 *       items a line is two items, so 126 of them.
 */
TEST(SegmentedPipeGeometry, DefaultSegmentIs256KB) {
    using engine_pipe           = qb::allocator::segmented_pipe<Item>;
    using engine_pool           = engine_pipe::pool_type;
    constexpr std::size_t items = (256u * 1024u) / sizeof(Item);
    static_assert(engine_pool::line_items == 64 / sizeof(Item));
    static_assert(engine_pool::stagger_slots == 64);
    static_assert(engine_pool::stagger_stride == 27);
    static_assert(engine_pool::stagger_slot(0) == 0, "the first carved segment is unstaggered");
    static_assert(engine_pool::stagger_slot(45) == 63, "the widest stagger is reached");
    static_assert(engine_pool::max_stagger == 63 * engine_pool::line_items);
    EXPECT_EQ(engine_pipe::segment_capacity, items - 1 - engine_pool::max_stagger);
    engine_pipe::pool_type pool;
    {
        engine_pipe p(pool);
        Item *const r = p.allocate_back(engine_pipe::segment_capacity);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r) % alignof(Item), 0u);
        EXPECT_EQ(p.segments(), 1u);
        // The first carved segment has stagger 0, so it holds the raw capacity: the widest
        // stagger's worth still fits, and only the item after that links the next segment.
        (void) p.allocate_back(engine_pool::max_stagger);
        EXPECT_EQ(p.segments(), 1u) << "the unstaggered first segment holds the raw capacity";
        (void) p.allocate_back(1);
        EXPECT_EQ(p.segments(), 2u) << "one item past a full segment links the next";
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// STAGGER: consecutive segments never share a page offset
// =============================================================================

/**
 * @test The `i`-th segment a pool carves starts `(i * 27) % 64` cache lines after its header, so
 *       two consecutive segments — what the two pipes of one core hold — sit 27 lines apart in
 *       their low twelve address bits, item `k` of one never 4K-aliases item `k` of the other,
 *       and no item near `k` does either: any two of six consecutively carved segments are at
 *       least 7 lines apart, circularly, so the reply's store never lands on the load the
 *       receive loop issues next. Every segment still accepts the guaranteed
 *       `segment_capacity` in ONE range (a request that wide is pooled, never dedicated), and a
 *       dedicated segment carries no stagger.
 */
TEST(SegmentedPipeStagger, ConsecutiveSegmentsDifferInPageOffset) {
    using engine_pipe = qb::allocator::segmented_pipe<Item>;
    using engine_pool = engine_pipe::pool_type;
    static_assert(engine_pool::stagger_slots > 1, "the default geometry must be able to stagger");
    engine_pool pool;
    {
        engine_pipe                 p(pool);
        std::vector<std::uintptr_t> starts;
        std::set<std::uintptr_t>    page_offsets;
        constexpr std::size_t       slots = engine_pool::stagger_slots;
        for (std::size_t i = 0; i < slots; ++i) {
            // Exactly one segment per request: the guaranteed width fills a segment whatever its
            // stagger, so the next request links the next carved segment.
            Item *const r = p.allocate_back(engine_pipe::segment_capacity);
            starts.push_back(reinterpret_cast<std::uintptr_t>(r));
            page_offsets.insert(reinterpret_cast<std::uintptr_t>(r) % 4096u);
            EXPECT_EQ(p.segments(), i + 1) << "request " << i << " must link exactly one segment";
        }
        EXPECT_EQ(page_offsets.size(), slots) << "every stagger slot must land on a distinct page offset";
        for (std::size_t i = 1; i < slots; ++i) {
            // Stagger advances by 27 cache lines per carved segment (mod a page), on top of the
            // segment stride: 27 forward, or 37 back, is 1728 bytes either way modulo 4096.
            EXPECT_EQ((starts[i] - starts[i - 1]) % 4096u, 27u * 64u) << "segments " << i - 1 << " and " << i;
        }
        for (std::size_t i = 0; i < slots; ++i) {
            for (std::size_t k = 1; k <= 5 && i + k < slots; ++k) {
                // Circular distance in lines between two segments carved up to five apart.
                auto const a = (starts[i] % 4096u) / 64u, b = (starts[i + k] % 4096u) / 64u;
                auto const d = a > b ? a - b : b - a;
                EXPECT_GE(std::min(d, 64u - d), 7u) << "segments " << i << " and " << i + k;
            }
        }
        // Every segment holds `segment_capacity` plus whatever its own stagger left of the widest
        // one: the tail (slot 37 of 64) still takes that much in place, and one item more links
        // a new segment.
        constexpr std::size_t spare = engine_pool::max_stagger - engine_pool::stagger_slot(slots - 1) * engine_pool::line_items;
        static_assert(spare > 0);
        (void) p.allocate_back(spare);
        EXPECT_EQ(p.segments(), slots);
        (void) p.allocate_back(1);
        EXPECT_EQ(p.segments(), slots + 1);
    }
    EXPECT_EQ(pool.outstanding(), 0u);
    {
        // A dedicated segment is exact and unstaggered: its range starts right after the header.
        engine_pipe p(pool);
        Item *const r = p.allocate_back(engine_pipe::segment_capacity + 1);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r) % alignof(Item), 0u);
        EXPECT_EQ(p.segments(), 1u);
        p.pop_front();
        EXPECT_EQ(p.segments(), 0u) << "a dedicated segment is freed, not kept resident";
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test The stagger survives the pool round trip: a segment released and re-acquired keeps the
 *       stagger it was carved with, so a pipe that empties and refills every pass — the swap pair
 *       of a `VirtualCore` — keeps its two segments on distinct page offsets for ever, whichever
 *       pipe holds which.
 */
TEST(SegmentedPipeStagger, StaggerIsAPropertyOfTheSegmentNotThePipe) {
    using engine_pipe = qb::allocator::segmented_pipe<Item>;
    engine_pipe::pool_type pool;
    {
        engine_pipe a(pool), b(pool);
        auto const  off = [](Item const *const r) {
            return reinterpret_cast<std::uintptr_t>(r) % 4096u;
        };
        auto const oa = off(a.allocate_back(1));
        auto const ob = off(b.allocate_back(1));
        EXPECT_NE(oa, ob) << "the two pipes' first segments must not share a page offset";
        for (int pass = 0; pass < 8; ++pass) {
            // Consume both, swap them (what `__receive__` does), refill: the offsets must stay the
            // same two values, exchanged or not, never collapse onto one.
            a.pop_front();
            b.pop_front();
            a.swap(b);
            auto const na = off(a.allocate_back(1));
            auto const nb = off(b.allocate_back(1));
            EXPECT_NE(na, nb) << "pass " << pass;
            EXPECT_TRUE((na == oa && nb == ob) || (na == ob && nb == oa)) << "pass " << pass;
        }
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

/**
 * @test A geometry too small to stagger degrades to a single slot: the test geometry (15 items
 *       of 32 bytes after the header) cannot spare a quarter of itself, so every segment starts
 *       right after its header and `segment_capacity` is the raw capacity, unchanged.
 */
TEST(SegmentedPipeStagger, TooSmallToStaggerMeansNoStagger) {
    static_assert(pool_t::stagger_slots == 1);
    static_assert(pool_t::max_stagger == 0);
    static_assert(pool_t::segment_capacity == pool_t::raw_capacity);
    static_assert(kCap == kSegmentItems - 1);
    pool_t pool;
    {
        pipe_t     p(pool);
        auto const first  = reinterpret_cast<std::uintptr_t>(p.allocate_back(kCap));
        auto const second = reinterpret_cast<std::uintptr_t>(p.allocate_back(kCap));
        EXPECT_EQ(p.segments(), 2u);
        EXPECT_EQ((second - first) % pool_t::segment_bytes, 0u) << "no stagger: segments differ by whole strides only";
    }
    EXPECT_EQ(pool.outstanding(), 0u);
}

// =============================================================================
// SLABS: where standard segments come from, and where they go back to
// =============================================================================

/**
 * @test A pool carves its standard segments out of one 2 MB slab from the process-wide
 *       `slab_cache`, hands the slab back WHOLE when it is destroyed, and the next pool gets that
 *       same slab from the cache without a new mapping. This is the warm-across-engines half of
 *       finding 9.11's A/B: the second engine of a process must not re-fault its pipes.
 */
TEST(SegmentedPipeSlab, PoolCarvesFromASlabAndReturnsItToTheCache) {
    using cache = qb::allocator::slab_cache;
    static_assert(pool_t::segments_per_slab == cache::slab_bytes / (kSegmentItems * sizeof(Item)));
    auto const cached_before   = cache::cached();
    auto const mappings_before = cache::mappings();
    auto const mappings_after  = cached_before == 0 ? mappings_before + 1 : mappings_before;
    void      *slab            = nullptr;
    {
        pool_t                   pool;
        pipe_t                   p(pool);
        std::set<std::uintptr_t> starts;
        for (std::uint64_t i = 0; i < 50 * kCap; ++i) {
            Item *const r = push_run(p, 1, i);
            starts.insert(reinterpret_cast<std::uintptr_t>(r) & ~static_cast<std::uintptr_t>(pool_t::segment_bytes - 1));
        }
        EXPECT_EQ(p.segments(), 50u);
        EXPECT_EQ(pool.slabs(), 1u) << "50 segments of " << pool_t::segment_bytes << " B fit one slab";
        // Every segment sits at a segment_bytes multiple inside one slab_bytes-sized window.
        ASSERT_EQ(starts.size(), 50u) << "segments do not overlap";
        auto const lo = *starts.begin(), hi = *starts.rbegin();
        EXPECT_LT(hi - lo, cache::slab_bytes);
        slab = reinterpret_cast<void *>(lo);
        EXPECT_EQ(cache::cached(), cached_before == 0 ? 0u : cached_before - 1)
            << "the slab came from the cache when one was there, from a fresh mapping otherwise";
        EXPECT_EQ(cache::mappings(), mappings_after);
        p.release_all();
        EXPECT_EQ(pool.outstanding(), 0u);
        EXPECT_EQ(pool.retained(), 50u);
    }
    // Pool gone: its slab is on the cache's free list, still mapped.
    auto const cached_now = cache::cached();
    EXPECT_GE(cached_now, 1u);
    {
        pool_t      pool;
        pipe_t      p(pool);
        Item *const r = push_run(p, 1, 0);
        EXPECT_EQ(cache::cached(), cached_now - 1) << "the new pool reused a cached slab";
        EXPECT_EQ(cache::mappings(), mappings_after) << "no new mapping";
        auto const base = reinterpret_cast<std::uintptr_t>(r) & ~static_cast<std::uintptr_t>(pool_t::segment_bytes - 1);
        EXPECT_EQ(reinterpret_cast<void *>(base), slab) << "LIFO: the slab the previous pool just released";
        p.release_all();
    }
    EXPECT_EQ(cache::cached(), cached_now);
}

/**
 * @test `shrink()` releases a slab only when NONE of its segments is lent: one live pipe holding
 *       one segment pins the slab, its retained siblings stay retained, and the slab goes back
 *       the moment the last lent segment does. A second slab that is entirely free goes at once:
 *       the walk is per slab, not all-or-nothing.
 */
TEST(SegmentedPipeSlab, ShrinkReleasesOnlyWhollyFreeSlabs) {
    using cache                    = qb::allocator::slab_cache;
    constexpr std::size_t per_slab = pool_t::segments_per_slab;
    pool_t                pool;
    pipe_t                a(pool), b(pool);
    // `a` fills the first slab entirely and spills into a second; `b` then takes one more.
    for (std::uint64_t i = 0; i < per_slab * kCap; ++i)
        (void) push_run(a, 1, i);
    EXPECT_EQ(a.segments(), per_slab);
    EXPECT_EQ(pool.slabs(), 1u);
    (void) push_run(a, 1, 0);
    EXPECT_EQ(pool.slabs(), 2u) << "one segment past a slab carves the next";
    (void) push_run(b, 1, 0);
    // Drain `a` completely: its per_slab + 1 segments are retained, `b`'s one is lent.
    a.release_all();
    EXPECT_EQ(pool.retained(), per_slab + 1);
    EXPECT_EQ(pool.outstanding(), 1u);
    auto const cached_before = cache::cached();
    pool.shrink();
    EXPECT_EQ(pool.slabs(), 1u) << "the wholly free first slab went back; b's slab is pinned";
    EXPECT_EQ(cache::cached(), cached_before + 1);
    EXPECT_EQ(pool.retained(), 1u) << "a's segment in the pinned slab stays retained";
    EXPECT_EQ(pool.outstanding(), 1u);
    // The pinned slab keeps serving.
    (void) push_run(b, 1, 1);
    EXPECT_EQ(pool.slabs(), 1u);
    b.release_all();
    EXPECT_EQ(pool.outstanding(), 0u);
    pool.shrink();
    EXPECT_EQ(pool.slabs(), 0u);
    EXPECT_EQ(pool.retained(), 0u);
    EXPECT_EQ(cache::cached(), cached_before + 2);
}

/**
 * @test `slab_cache::trim()` unmaps exactly the cached slabs and never a lent one, and the
 *       platform geometry holds: 2 MB slabs, aligned to 2 MB on POSIX (the huge-page
 *       precondition) and to the 64 KB allocation granularity on Windows; on a Linux kernel
 *       that has `MADV_POPULATE_WRITE` (5.14+) a fresh slab reports itself prefaulted.
 */
TEST(SegmentedPipeSlab, TrimUnmapsCachedSlabsOnlyAndGeometryHolds) {
    using cache = qb::allocator::slab_cache;
    EXPECT_EQ(cache::slab_bytes, 2u * 1024u * 1024u);
#if defined(_WIN32) || defined(_WIN64)
    EXPECT_EQ(cache::alignment(), 64u * 1024u);
#else
    EXPECT_EQ(cache::alignment(), cache::slab_bytes);
#endif
    pool_t      pool;
    pipe_t      p(pool);
    Item *const r = push_run(p, 1, 0);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r) % alignof(Item), 0u);
    // The first segment carved from a slab starts AT the slab, and its items follow the one-item
    // header (`segment_header` is three words, `Item` is 32 bytes) — so the first item sits one
    // `Item` past a slab-aligned address.
    EXPECT_EQ((reinterpret_cast<std::uintptr_t>(r) - sizeof(Item)) % cache::alignment(), 0u);
    // Make sure at least one slab is cached, then trim: only the cached ones go.
    {
        pool_t other;
        pipe_t q(other);
        (void) push_run(q, 1, 0);
        q.release_all();
    }
    auto const cached = cache::cached();
    ASSERT_GE(cached, 1u);
    auto const mapped = cache::mapped();
    EXPECT_EQ(cache::trim(), cached);
    EXPECT_EQ(cache::cached(), 0u);
    EXPECT_EQ(cache::mapped(), mapped - cached);
    EXPECT_EQ(cache::trim(), 0u);
    // The lent slab is untouched: its segment is still writable and its pool still whole.
    r->value = 42;
    EXPECT_EQ(p.front().data()->value, 42u);
    EXPECT_EQ(pool.slabs(), 1u);
    p.release_all();
    EXPECT_EQ(pool.outstanding(), 0u);
#if defined(__linux__)
    // The cache is empty after the trim, so this pool maps a fresh slab: the flag reflects THIS
    // kernel. Read it against the kernel's own version.
    {
        pool_t fresh;
        pipe_t q(fresh);
        (void) push_run(q, 1, 0);
        q.release_all();
    }
    struct utsname u{};
    ASSERT_EQ(::uname(&u), 0);
    int major = 0, minor = 0;
    ASSERT_EQ(std::sscanf(u.release, "%d.%d", &major, &minor), 2);
    if (major > 5 || (major == 5 && minor >= 14))
        EXPECT_TRUE(cache::prefaulted()) << "kernel " << u.release << " has MADV_POPULATE_WRITE";
    else
        std::printf("[   note   ] kernel %s predates MADV_POPULATE_WRITE; slabs fault on first touch\n", u.release);
#endif
}
