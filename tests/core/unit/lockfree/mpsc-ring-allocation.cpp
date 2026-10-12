/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/lockfree/mpsc-ring-allocation.cpp
 * @brief The runtime-producer `mpsc::ringbuffer` allocates a producer's ring on that producer's first enqueue, and
 *        on nothing else (Huly QB-203).
 *
 * `mpsc::ringbuffer<T, max_size, 0>` used to value-initialise a vector of all its producers' rings at construction --
 * what the engine's mailboxes were until 3.3, N of them of N rings each, every byte written at `Main::start()`. It now
 * holds one slot per producer and constructs a ring the first time its producer enqueues -- on POSIX in its place in
 * one block the buffer takes at construction, untouched until then; on Windows in a region of its own. The engine's
 * `Mailbox` is no longer this type but `mpsc::block_ringbuffer`, every ring built at once
 * (`qb-core-test-unit-mpsc-block-ring`); what an engine holds before its first event is
 * `qb-core-test-system-mailbox-footprint`.
 *
 * What this file pins:
 *   - construction allocates no ring, and neither do the consumer's walks (`has_data`, both `consume_all`, `dequeue`);
 *   - each producing form -- `enqueue(index, ...)` single and bulk, `enqueue_at<Index>`, the producer-side `ringOf` --
 *     allocates that producer's ring and no other, once;
 *   - rings published while the consumer is already walking deliver every item, once and in order per producer: the
 *     slot's compare-exchange against the consumer's acquire load, which ThreadSanitizer checks;
 *   - engine-sized rings (66 176 bytes) live in `region_cache` regions (Huly QB-1012) -- on POSIX one per buffer, taken
 *     at construction, constructing a ring in it lending nothing more; on Windows one per ring: the rings of one buffer
 *     start at distinct offsets in their pages, the next buffer builds in the regions the previous one left, mapping
 *     nothing, and threads racing for one producer's ring share one ring, constructed once -- on POSIX the losers of
 *     the slot's claim build nothing, on Windows they give back what they built.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/main.h>
#include <qb/system/allocator/region.h>
#include <qb/system/lockfree/mpsc.h>

namespace mpsc_ring_allocation_test {

constexpr std::size_t kCap = 63; // a 64-slot ring: the publication case's producers find theirs full and retry
using Ring                 = qb::lockfree::mpsc::ringbuffer<std::uint64_t, kCap, 0>;

constexpr std::size_t   kProducers  = 64;
constexpr std::size_t   kPublishers = 8;
constexpr std::uint64_t kItemsEach  = 4 * kCap;
constexpr int           kRounds     = 32;

/// An item that says which producer sent it and in what order.
[[nodiscard]] constexpr std::uint64_t
item(std::size_t const producer, std::uint64_t const seq) noexcept {
    return (static_cast<std::uint64_t>(producer) << 32) | seq;
}

TEST(MpscRingAllocation, ARingIsAllocatedByItsProducersFirstEnqueueAndByNothingElse) {
    Ring ring(kProducers);
    EXPECT_EQ(ring.rings(), 0u) << "construction allocates one slot per producer and no ring";

    std::vector<std::uint64_t> scratch(kCap);
    EXPECT_FALSE(ring.has_data());
    EXPECT_EQ(ring.dequeue(scratch.data(), scratch.size()), 0u);
    EXPECT_EQ(ring.consume_all([](std::uint64_t *, std::size_t) { ADD_FAILURE() << "an empty buffer delivered a batch"; }, scratch.data(),
                               scratch.size()),
              0u);
    EXPECT_EQ(ring.consume_all([](std::uint64_t *, std::size_t) { ADD_FAILURE() << "an empty buffer delivered items"; }), 0u);
    EXPECT_EQ(ring.rings(), 0u) << "the consumer's walks allocate nothing";

    ASSERT_TRUE(ring.enqueue(37, item(37, 0)));
    EXPECT_EQ(ring.rings(), 1u) << "producer 37's first enqueue allocated its ring, and no other";
    ASSERT_TRUE(ring.enqueue(37, item(37, 1)));
    EXPECT_EQ(ring.rings(), 1u) << "and only once";
    EXPECT_TRUE(ring.has_data());

    std::vector<std::uint64_t> seen;
    EXPECT_EQ(ring.consume_all([&seen](std::uint64_t *items, std::size_t const count) { seen.insert(seen.end(), items, items + count); },
                               scratch.data(), scratch.size()),
              2u);
    EXPECT_EQ(seen, (std::vector<std::uint64_t>{item(37, 0), item(37, 1)}));
    EXPECT_FALSE(ring.has_data());

    ASSERT_TRUE(ring.enqueue_at<5>(item(5, 0)));
    EXPECT_EQ(ring.rings(), 2u) << "the compile-time index allocates like the runtime one";
    std::uint64_t const bulk[2] = {item(9, 0), item(9, 1)};
    ASSERT_EQ(ring.enqueue(9, bulk, 2), 2u);
    EXPECT_EQ(ring.rings(), 3u) << "and so does the bulk form";

    (void) ring.ringOf(3); // the producer-side handle allocates as a first enqueue would
    EXPECT_EQ(ring.rings(), 4u);
    ASSERT_TRUE(ring.enqueue(3, item(3, 0)));
    EXPECT_EQ(ring.rings(), 4u) << "the enqueue uses the ring ringOf allocated";

    std::vector<std::uint64_t> out(8);
    ASSERT_EQ(ring.dequeue(out.data(), out.size()), 4u);
    out.resize(4);
    EXPECT_EQ(out, (std::vector<std::uint64_t>{item(3, 0), item(5, 0), item(9, 0), item(9, 1)})) << "producers drain in index order";
}

TEST(MpscRingAllocation, RingsPublishedWhileTheConsumerWalksDeliverEveryItemInOrder) {
    // Each round, the consumer walks an empty buffer first, then releases eight producers that allocate and publish their
    // rings while it keeps walking. A lost or early publication would show as an item never seen or seen out of order;
    // the consumer stops once everything published has been drained, whatever arrived, so a lost item cannot hang it.
    for (int round = 0; round < kRounds; ++round) {
        Ring                     ring(kPublishers);
        std::latch               go(1);
        std::atomic<std::size_t> finished{0};

        std::vector<std::thread> producers;
        producers.reserve(kPublishers);
        for (std::size_t p = 0; p < kPublishers; ++p)
            producers.emplace_back([&ring, &go, &finished, p] {
                go.wait();
                for (std::uint64_t seq = 0; seq < kItemsEach;) {
                    if (ring.enqueue(p, item(p, seq)))
                        ++seq;
                    else
                        std::this_thread::yield(); // full: the consumer drains it
                }
                finished.fetch_add(1, std::memory_order_release);
            });

        std::vector<std::uint64_t> scratch(kCap);
        std::vector<std::uint64_t> next(kPublishers, 0);
        std::size_t                out_of_order = 0;
        std::size_t                foreign      = 0;
        std::size_t                received     = 0;
        auto const                 walk         = [&] {
            return ring.consume_all(
                [&](std::uint64_t *items, std::size_t const count) {
                    for (std::size_t i = 0; i < count; ++i) {
                        const std::uint64_t producer = items[i] >> 32;
                        const std::uint64_t seq      = items[i] & 0xffffffffu;
                        if (producer >= kPublishers) {
                            ++foreign;
                            continue;
                        }
                        if (seq != next[producer])
                            ++out_of_order;
                        next[producer] = seq + 1;
                    }
                },
                scratch.data(), scratch.size());
        };

        EXPECT_EQ(walk(), 0u);
        go.count_down(); // the consumer is walking; the producers publish now
        for (;;) {
            const bool        all_published = finished.load(std::memory_order_acquire) == kPublishers; // BEFORE the walk
            const std::size_t drained       = walk();
            received += drained;
            if (received == kPublishers * kItemsEach || (all_published && drained == 0))
                break;
        }
        for (auto &producer : producers)
            producer.join();

        ASSERT_EQ(received, kPublishers * kItemsEach) << "round " << round;
        ASSERT_EQ(out_of_order, 0u) << "round " << round;
        ASSERT_EQ(foreign, 0u) << "round " << round;
        for (std::size_t p = 0; p < kPublishers; ++p)
            ASSERT_EQ(next[p], kItemsEach) << "round " << round << ", producer " << p;
        ASSERT_EQ(ring.rings(), kPublishers) << "round " << round;
        ASSERT_FALSE(ring.has_data()) << "round " << round;
    }
}

// ---- the engine's ring: in a region of the region cache -----------------------------------------------------------------

namespace region = qb::allocator::detail;

/// Engine-sized rings: 66 176-byte producers, in one `region_cache` region per buffer (POSIX) or per ring.
using EngineRing                   = qb::lockfree::mpsc::ringbuffer<EventBucket, qb::detail::max_deliverable_buckets, 0>;
constexpr std::size_t kEngineRings = 8; // fewer than the stagger's 28 slots: one buffer's rings all differ
constexpr std::size_t kRacers      = 8;

/// Where producer @p index's ring lies within its page: what the stagger moves.
[[nodiscard]] std::size_t
page_offset(EngineRing &ring, std::size_t const index) {
    return reinterpret_cast<std::uintptr_t>(&ring.ringOf(index)) % region::region_page_bytes;
}

TEST(MpscRingAllocation, AnEngineRingIsInARegionAtItsOwnPageOffsetAndTheNextBufferBuildsInTheRegionsTheLastOneLeft) {
    std::size_t const lent_before = region::region_stats().lent_bytes;
    {
        EngineRing            ring(qb::MaxCores);
        std::size_t const     lent_block = region::region_stats().lent_bytes - lent_before;
        std::set<std::size_t> offsets;
        for (std::size_t i = 0; i < kEngineRings; ++i)
            offsets.insert(page_offset(ring, i));
        ASSERT_EQ(ring.rings(), kEngineRings);
        EXPECT_EQ(offsets.size(), kEngineRings) << "the rings of one buffer start at the same offset in their pages: every ring's "
                                                   "index lines in one L1 set and one 4K-aliasing class (the stagger is gone)";
        std::size_t const lent_rings = region::region_stats().lent_bytes - lent_before - lent_block;
#if defined(_WIN32)
        EXPECT_EQ(lent_block, 0u) << "a buffer took storage for rings no producer has asked for";
        EXPECT_EQ(lent_rings % kEngineRings, 0u);
        EXPECT_GE(lent_rings / kEngineRings, region::region_min_bytes) << "an engine-sized ring is not a region_cache region";
#else
        EXPECT_GE(lent_block, qb::MaxCores * region::region_min_bytes) << "the buffer's block is not a region_cache region with "
                                                                          "room for every producer's ring";
        EXPECT_EQ(lent_rings, 0u) << "a ring constructed in the block took storage of its own";
#endif
    }
    EXPECT_EQ(region::region_stats().lent_bytes, lent_before) << "a destroyed buffer gave its regions back";

    // The next buffer -- its stagger base is the next one -- builds its rings in the regions the first one left.
    std::size_t const mappings_before = region::region_stats().mappings;
    EngineRing        next(qb::MaxCores);
    for (std::size_t i = 0; i < kEngineRings; ++i)
        (void) next.ringOf(i);
    EXPECT_EQ(region::region_stats().mappings, mappings_before) << "a ring of the next buffer was mapped afresh instead of built in "
                                                                   "the region a destroyed one left in the cache";
}

TEST(MpscRingAllocation, ThreadsRacingForOneEngineRingShareOneRingConstructedOnce) {
    EngineRing                  ring(4);
    std::size_t const           lent_before = region::region_stats().lent_bytes;
    std::latch                  go(kRacers);
    std::vector<std::uintptr_t> seen(kRacers, 0);
    std::vector<std::thread>    racers;
    racers.reserve(kRacers);
    for (std::size_t t = 0; t < kRacers; ++t)
        racers.emplace_back([&ring, &go, &seen, t] {
            go.arrive_and_wait(); // all at once: one wins the slot, the others wait for (POSIX) or give back (Windows) theirs
            seen[t] = reinterpret_cast<std::uintptr_t>(&ring.ringOf(2));
        });
    for (auto &racer : racers)
        racer.join();

    ASSERT_EQ(ring.rings(), 1u);
    for (std::size_t t = 0; t < kRacers; ++t)
        EXPECT_EQ(seen[t], seen[0]) << "racer " << t << " was handed a ring that is not the published one";
    std::size_t const one_ring = region::region_stats().lent_bytes - lent_before;
    (void) ring.ringOf(3); // uncontended: exactly one region more (Windows), nothing more (POSIX: in the block)
    EXPECT_EQ(region::region_stats().lent_bytes - lent_before, 2 * one_ring)
        << "the race left more than the winner's ring lent: a loser kept the ring it built";
#if !defined(_WIN32)
    EXPECT_EQ(one_ring, 0u) << "a ring constructed in the buffer's block took storage of its own";
#endif
}

} // namespace mpsc_ring_allocation_test
