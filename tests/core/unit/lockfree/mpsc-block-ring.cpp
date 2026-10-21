/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/lockfree/mpsc-block-ring.cpp
 * @brief `mpsc::block_ringbuffer`, the engine's mailbox: every producer's ring built by the constructor in one block and
 *        read where the block holds it; on Windows each ring's header committed and the ring built CLOSED, its slots
 *        committed by its producer's first write (Huly QB-203).
 *
 * Until 3.3 the engine's mailbox was the runtime-producer `mpsc::ringbuffer`, its rings a vector value-initialised at
 * construction: every byte of N x N rings written at `Main::start()`. The mailbox is now a `block_ringbuffer`: the same
 * rings at the same stride, read the same way, each constructed without its slot array.
 *
 * What this file pins:
 *   - a block holds every producer's ring from its construction, at one stride from the block's first byte, and its
 *     producing forms and consumer walks are the vector's: each form writes its own index's ring, the consumer drains in
 *     index order;
 *   - the CLOSED protocol, on every platform -- `block_ringbuffer<T, N, true>` builds a POSIX block closed, which commits
 *     nothing there and runs the protocol where ThreadSanitizer sees it: a closed ring is empty to its consumer (`has_data`, both
 *     `consume_all`, `dequeue`, `empty()`) and full to its producer until its first write, which each producing form
 *     and `write_room` through `ringOf` open, and a zero-length query does not; an opened ring fills and drains as any;
 *   - producers opening their rings while the consumer walks: every item delivered once and in order, nothing read from
 *     a closed ring, in a block the region cache lends again each round with the last round's items in its slots;
 *   - two producers never race one ring's first write: a ring has ONE producer by construction -- the indexed forms
 *     are one thread per index, and the engine passes each sender core's own resolved index -- and the round-robin forms,
 *     which any thread may call, open a ring under its producer's lock: eight writers on one closed ring;
 *   - the engine's `Mailbox` is a block: every ring at construction, built closed on Windows only;
 *   - Windows: a mailbox's block holds its rings' header pages committed and nothing else until a producer writes, then
 *     exactly the pages of that ring's slots more -- read from the block's own pages with `VirtualQuery`, which counts
 *     what is committed, not what was touched -- and the next mailbox of its size is built in that block, mapping
 *     nothing, keeping what was committed, every ring closed and empty again.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <qb/main.h>
#include <qb/system/allocator/region.h>
#include <qb/system/lockfree/mpsc.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mpsc_block_ring_test {

using namespace std::chrono_literals;

constexpr std::size_t kCap = 63; // a 64-slot ring: the concurrent cases' producers find theirs full and retry
using Block                = qb::lockfree::mpsc::block_ringbuffer<std::uint64_t, kCap>;       // the platform's: closed on Windows
using ClosedBlock          = qb::lockfree::mpsc::block_ringbuffer<std::uint64_t, kCap, true>; // the protocol, on every platform

constexpr std::size_t   kProducers  = 16;
constexpr std::size_t   kPublishers = 8;
constexpr std::size_t   kWriters    = 8;
constexpr std::uint64_t kItemsEach  = 4 * kCap;
constexpr int           kRounds     = 32;
/// How long the consumer of a concurrent case waits with nothing to take before it calls the round lost: a protocol that
/// refuses a ring's producer for good -- a ring never opened, a consumer's index over the closed mark -- fails the case
/// in that time instead of hanging the binary.
constexpr auto kStall = std::chrono::seconds(10);

/// An item that says in which round, from which producer and in what order it was sent.
[[nodiscard]] constexpr std::uint64_t
item(std::size_t const producer, std::uint64_t const seq, std::uint64_t const round = 0) noexcept {
    return (round << 48) | (static_cast<std::uint64_t>(producer) << 32) | seq;
}

/// What producer @p index last saw of its consumer's index: one past its write index in a ring built closed -- full to
/// its producer -- and the consumer's true index once the ring has opened. Producer side; here, one thread.
template <typename B>
[[nodiscard]] std::size_t
seen_read_index(B &block, std::size_t const index) {
    return block.ringOf(index).producer_seen_read_index();
}

/// Every item a copy-out walk delivers, in delivery order.
template <typename B>
[[nodiscard]] std::vector<std::uint64_t>
drain(B &block) {
    std::vector<std::uint64_t> scratch(kCap);
    std::vector<std::uint64_t> items;
    block.consume_all([&items](std::uint64_t *batch, std::size_t const count) { items.insert(items.end(), batch, batch + count); },
                      scratch.data(), scratch.size());
    return items;
}

TEST(MpscBlockRing, ABlockHoldsEveryRingFromItsConstructionAtOneStrideAndDrainsThemInIndexOrder) {
    Block block(kProducers);
    EXPECT_EQ(block.rings(), kProducers);
#if defined(_WIN32)
    EXPECT_TRUE(block.closed()) << "a Windows block is reserved, not committed: its rings are built closed";
#else
    EXPECT_FALSE(block.closed()) << "a POSIX block is built open unless asked";
#endif
    auto const        base   = reinterpret_cast<std::uintptr_t>(block.block());
    std::size_t const stride = block.block_bytes() / kProducers;
    auto const        first  = reinterpret_cast<std::uintptr_t>(&block.ringOf(0)) - base; // past producer 0's lock line
    EXPECT_EQ(base % qb::allocator::detail::region_page_bytes, 0u) << "the block is a region: page-aligned";
    for (std::size_t i = 0; i < kProducers; ++i)
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&block.ringOf(i)) - base, i * stride + first) << "ring " << i << " is not at its place";

    EXPECT_FALSE(block.has_data());
    ASSERT_TRUE(block.enqueue(9, item(9, 0)));
    ASSERT_TRUE(block.enqueue_at<3>(item(3, 0)));
    std::uint64_t const bulk[2] = {item(5, 0), item(5, 1)};
    ASSERT_EQ(block.enqueue(5, bulk, 2), 2u);
    EXPECT_TRUE(block.has_data());
    std::vector<std::uint64_t> out(8);
    ASSERT_EQ(block.dequeue(out.data(), out.size()), 4u);
    out.resize(4);
    EXPECT_EQ(out, (std::vector<std::uint64_t>{item(3, 0), item(5, 0), item(5, 1), item(9, 0)})) << "producers drain in index order";
    EXPECT_FALSE(block.has_data());

    ASSERT_TRUE(block.enqueue(12, item(12, 0)));
    ASSERT_TRUE(block.enqueue(1, item(1, 0)));
    EXPECT_EQ(drain(block), (std::vector<std::uint64_t>{item(1, 0), item(12, 0)}));
    ASSERT_TRUE(block.enqueue(7, item(7, 0)));
    std::size_t in_place = 0;
    EXPECT_EQ(block.consume_all([&in_place](std::uint64_t *, std::size_t const count) { in_place += count; }), 1u);
    EXPECT_EQ(in_place, 1u);
}

TEST(MpscBlockRing, AClosedRingIsEmptyToItsConsumerAndItsProducersFirstWriteOpensIt) {
    ClosedBlock block(kProducers);
    ASSERT_TRUE(block.closed());
    for (std::size_t i = 0; i < kProducers; ++i) {
        EXPECT_TRUE(block.ringOf(i).empty()) << "closed ring " << i << " is not empty to its consumer";
        EXPECT_EQ(seen_read_index(block, i), 1u) << "closed ring " << i << " was not built full to its producer";
    }
    std::vector<std::uint64_t> scratch(kCap);
    EXPECT_FALSE(block.has_data());
    EXPECT_EQ(block.dequeue(scratch.data(), scratch.size()), 0u);
    EXPECT_EQ(block.consume_all([](std::uint64_t *, std::size_t) { ADD_FAILURE() << "a closed ring delivered a batch"; }, scratch.data(),
                                scratch.size()),
              0u);
    EXPECT_EQ(block.consume_all([](std::uint64_t *, std::size_t) { ADD_FAILURE() << "a closed ring delivered items"; }), 0u);

    // A query for no room proves nothing it has to look past its snapshot for: the ring stays closed.
    EXPECT_EQ(block.ringOf(4).write_room(0), 0u);
    EXPECT_EQ(seen_read_index(block, 4), 1u) << "a zero-length query opened a ring";

    // Each producing form opens its own index's ring, and no other.
    ASSERT_TRUE(block.enqueue(1, item(1, 0)));
    EXPECT_EQ(seen_read_index(block, 1), 0u) << "the first write did not open the ring: its producer sees the consumer's true index";
    ASSERT_TRUE(block.enqueue_at<2>(item(2, 0)));
    std::uint64_t const bulk[2] = {item(3, 0), item(3, 1)};
    ASSERT_EQ(block.enqueue(3, bulk, 2), 2u);
    EXPECT_EQ(block.ringOf(4).write_room(kCap), kCap) << "write_room through ringOf -- send_run's path -- opens the ring and finds it empty";
    std::uint64_t const run[2] = {item(4, 0), item(4, 1)};
    ASSERT_EQ(block.ringOf(4).enqueue<true>(run, 2), 2u);
    for (std::size_t i = 0; i < kProducers; ++i) {
        if (i == 0 || i > 4) {
            EXPECT_EQ(seen_read_index(block, i), 1u) << "ring " << i << " opened by another producer's write";
        }
    }

    EXPECT_EQ(drain(block), (std::vector<std::uint64_t>{item(1, 0), item(2, 0), item(3, 0), item(3, 1), item(4, 0), item(4, 1)}))
        << "the consumer drains the opened rings in index order, and nothing from a closed one";

    // An opened ring is a ring: its capacity fits, the next is refused, a drain makes room.
    for (std::uint64_t seq = 1; seq <= kCap; ++seq)
        ASSERT_TRUE(block.enqueue(1, item(1, seq))) << seq;
    EXPECT_FALSE(block.enqueue(1, item(1, kCap + 1))) << "a full opened ring took one more";
    EXPECT_EQ(drain(block).size(), kCap);
    EXPECT_TRUE(block.enqueue(1, item(1, kCap + 1)));
}

TEST(MpscBlockRing, EnqueueWithOpensOnlyItsClosedProducerBeforeFillingAndRefusesWithoutACallback) {
    ClosedBlock           block(kProducers);
    constexpr std::size_t producer = 5;
    auto                 &ring     = block.ringOf(producer);
    std::size_t           calls = 0, first_seen_read = 1, first_size = 0, second_size = 0;
    std::uint64_t         next = 0;
    auto const            fill = [&](std::uint64_t *first, std::size_t first_count, std::uint64_t *second, std::size_t second_count) noexcept {
        if (!calls)
            first_seen_read = seen_read_index(block, producer);
        ++calls;
        first_size  = first_count;
        second_size = second_count;
        for (std::size_t i = 0; i < first_count; ++i)
            first[i] = item(producer, next++);
        for (std::size_t i = 0; i < second_count; ++i)
            second[i] = item(producer, next++);
    };

    EXPECT_EQ(ring.enqueue_with(0, fill), 0u);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(seen_read_index(block, producer), 1u) << "an empty transaction must leave the ring closed";
    EXPECT_FALSE(block.has_data());

    ASSERT_EQ(ring.enqueue_with(kCap, fill), kCap);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(first_seen_read, 0u) << "opening, including Windows page commitment, precedes the callback";
    EXPECT_EQ(first_size, kCap);
    EXPECT_EQ(second_size, 0u);
    for (std::size_t i = 0; i < kProducers; ++i)
        if (i != producer)
            EXPECT_EQ(seen_read_index(block, i), 1u) << "another producer's ring was opened";
    EXPECT_EQ(ring.enqueue_with(1, fill), 0u) << "full";
    EXPECT_EQ(ring.enqueue_with(0, fill), 0u);
    EXPECT_EQ(calls, 1u);

    std::vector<std::uint64_t> expected;
    for (std::uint64_t seq = 0; seq < kCap; ++seq)
        expected.push_back(item(producer, seq));
    EXPECT_EQ(drain(block), expected);

    // The opened ring must retain normal snapshot refresh and wrap behaviour.
    ASSERT_EQ(ring.enqueue_with(2, fill), 2u);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(first_size, 1u);
    EXPECT_EQ(second_size, 1u);
    EXPECT_EQ(drain(block), (std::vector<std::uint64_t>{item(producer, kCap), item(producer, kCap + 1)}));
    EXPECT_FALSE(block.has_data());
}

TEST(MpscBlockRing, ProducersOpeningTheirRingsWhileTheConsumerWalksDeliverEveryItemOnceAndInOrder) {
    // Each round, a block of closed rings, the consumer walking it before and while eight producers open theirs. The
    // region cache lends each round the block the last one left, its slots holding that round's items: a closed ring
    // read as non-empty would deliver them, and they carry their round. A ring's first write and the consumer's polls of
    // its published index are what ThreadSanitizer checks.
    for (int round = 0; round < kRounds; ++round) {
        ClosedBlock              block(kPublishers);
        std::latch               go(1);
        std::atomic<std::size_t> finished{0};
        std::atomic<bool>        give_up{false};
        auto const               tag = static_cast<std::uint64_t>(round);

        std::vector<std::thread> producers;
        producers.reserve(kPublishers);
        for (std::size_t p = 0; p < kPublishers; ++p)
            producers.emplace_back([&block, &go, &finished, &give_up, p, tag] {
                go.wait();
                for (std::uint64_t seq = 0; seq < kItemsEach && !give_up.load(std::memory_order_relaxed);) {
                    if (block.enqueue(p, item(p, seq, tag)))
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
            return block.consume_all(
                [&](std::uint64_t *items, std::size_t const count) {
                    for (std::size_t i = 0; i < count; ++i) {
                        const std::uint64_t from     = items[i] >> 48;
                        const std::uint64_t producer = (items[i] >> 32) & 0xffffu;
                        const std::uint64_t seq      = items[i] & 0xffffffffu;
                        if (from != tag || producer >= kPublishers) {
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

        EXPECT_EQ(walk(), 0u) << "round " << round << ": a closed ring delivered";
        go.count_down(); // the consumer is walking; the producers open their rings now
        for (auto last = std::chrono::steady_clock::now();;) {
            const bool        all_done = finished.load(std::memory_order_acquire) == kPublishers; // BEFORE the walk
            const std::size_t drained  = walk();
            received += drained;
            if (received >= kPublishers * kItemsEach || (all_done && drained == 0))
                break;
            if (drained)
                last = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - last > kStall)
                break; // a producer refused for good: the round is lost, and says so below
        }
        give_up.store(true, std::memory_order_relaxed);
        for (auto &producer : producers)
            producer.join();

        ASSERT_EQ(received, kPublishers * kItemsEach) << "round " << round;
        ASSERT_EQ(foreign, 0u) << "round " << round << ": items of another round -- a closed ring read as non-empty";
        ASSERT_EQ(out_of_order, 0u) << "round " << round;
        for (std::size_t p = 0; p < kPublishers; ++p)
            ASSERT_EQ(next[p], kItemsEach) << "round " << round << ", producer " << p;
        ASSERT_FALSE(block.has_data()) << "round " << round;
    }
}

TEST(MpscBlockRing, RoundRobinWritersOpenAClosedRingOnceUnderItsLock) {
    // The round-robin forms are the only way two threads write one ring, and they take its producer's lock: eight
    // writers on a one-ring block make its first write at once, and the lock lets one open it.
    for (int round = 0; round < kRounds; ++round) {
        ClosedBlock              block(1);
        std::latch               go(kWriters);
        std::atomic<std::size_t> finished{0};
        std::atomic<bool>        give_up{false};
        std::vector<std::thread> writers;
        writers.reserve(kWriters);
        for (std::size_t w = 0; w < kWriters; ++w)
            writers.emplace_back([&block, &go, &finished, &give_up, w] {
                go.arrive_and_wait();
                for (std::uint64_t seq = 0; seq < kItemsEach && !give_up.load(std::memory_order_relaxed);) {
                    if (block.enqueue(item(w, seq)))
                        ++seq;
                    else
                        std::this_thread::yield();
                }
                finished.fetch_add(1, std::memory_order_release);
            });

        std::vector<std::uint64_t> next(kWriters, 0);
        std::size_t                out_of_order = 0;
        std::size_t                received     = 0;
        std::vector<std::uint64_t> scratch(kCap);
        for (auto last = std::chrono::steady_clock::now();;) {
            const bool        all_done = finished.load(std::memory_order_acquire) == kWriters;
            const std::size_t drained  = block.consume_all(
                [&](std::uint64_t *items, std::size_t const count) {
                    for (std::size_t i = 0; i < count; ++i) {
                        const std::uint64_t writer = items[i] >> 32;
                        const std::uint64_t seq    = items[i] & 0xffffffffu;
                        if (writer >= kWriters || seq != next[writer])
                            ++out_of_order;
                        else
                            next[writer] = seq + 1;
                    }
                },
                scratch.data(), scratch.size());
            received += drained;
            if (received >= kWriters * kItemsEach || (all_done && drained == 0))
                break;
            if (drained)
                last = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - last > kStall)
                break; // the ring refused its writers for good: the round is lost, and says so below
        }
        give_up.store(true, std::memory_order_relaxed);
        for (auto &writer : writers)
            writer.join();
        ASSERT_EQ(received, kWriters * kItemsEach) << "round " << round;
        ASSERT_EQ(out_of_order, 0u) << "round " << round << ": a writer's items out of order in the ring they share";
    }
}

TEST(MpscBlockRing, TheEnginesMailboxIsABlockWithEveryRingBuiltClosedOnWindowsOnly) {
    qb::SharedCoreCommunication::Mailbox mailbox(qb::MaxCores, 0ns, 0us);
    EXPECT_EQ(mailbox.rings(), qb::MaxCores);
    // An open block's ring is the plain fixed ring -- develop's code, no test of the closed mark on any write; only a
    // closed block's is the closable one.
    using MailboxRing            = std::remove_reference_t<decltype(mailbox.ringOf(0))>;
    constexpr std::size_t kSlots = qb::detail::max_deliverable_buckets;
#if defined(_WIN32)
    EXPECT_TRUE(mailbox.closed());
    static_assert(std::is_same_v<MailboxRing, qb::lockfree::spsc::internal::closable_ringbuffer<EventBucket, kSlots>>);
#else
    EXPECT_FALSE(mailbox.closed());
    static_assert(std::is_same_v<MailboxRing, qb::lockfree::spsc::ringbuffer<EventBucket, kSlots>>,
                  "a POSIX engine's mailbox ring carries the closed protocol");
#endif
    EXPECT_FALSE(mailbox.has_data());
}

#if defined(_WIN32)

// ---- Windows: what a mailbox's block commits ------------------------------------------------------------------------

constexpr std::size_t kPage = qb::allocator::detail::region_page_bytes;
using Mailbox               = qb::SharedCoreCommunication::Mailbox;
/// A ring's index blocks, all a ring's constructor writes past its producer's lock line; its slots follow them.
constexpr std::size_t kIndexBytes = sizeof(qb::lockfree::spsc::internal::ringbuffer<EventBucket>);
static_assert(kIndexBytes % alignof(EventBucket) == 0, "a ring's slots start where its index blocks end");

/// The pages of [`base`, `base + bytes`) the process has committed, by index from `base`.
[[nodiscard]] std::set<std::size_t>
committed_pages(void const *const base, std::size_t const bytes) {
    std::set<std::size_t> pages;
    auto const            begin = reinterpret_cast<std::uintptr_t>(base);
    auto const            end   = begin + (bytes + kPage - 1) / kPage * kPage;
    for (std::uintptr_t at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<void const *>(at), &info, sizeof(info)) == 0) {
            ADD_FAILURE() << "VirtualQuery refused " << at;
            break;
        }
        std::uintptr_t const stop = (std::min) (reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize, end);
        if (info.State == MEM_COMMIT)
            for (std::uintptr_t page = at; page < stop; page += kPage)
                pages.insert((page - begin) / kPage);
        at = stop;
    }
    return pages;
}

/// The pages [`from`, `to`) spans, by index from `base`.
[[nodiscard]] std::set<std::size_t>
pages_of(std::uintptr_t const base, std::uintptr_t const from, std::uintptr_t const to) {
    std::set<std::size_t> pages;
    for (std::uintptr_t page = (from - base) / kPage; page <= (to - 1 - base) / kPage; ++page)
        pages.insert(page);
    return pages;
}

/// Where producer @p index's ring lies in a mailbox's block: its header -- the lock line and the index blocks -- then its
/// slots, to the next producer's lock line.
struct RingPages {
    std::set<std::size_t> header;
    std::set<std::size_t> slots;
};

[[nodiscard]] RingPages
ring_pages(Mailbox &mailbox, std::size_t const index) {
    auto const        base   = reinterpret_cast<std::uintptr_t>(mailbox.block());
    std::size_t const stride = mailbox.block_bytes() / mailbox.rings();
    auto const        start  = base + index * stride;
    auto const        slots  = reinterpret_cast<std::uintptr_t>(&mailbox.ringOf(index)) + kIndexBytes;
    return {pages_of(base, start, slots), pages_of(base, slots, start + stride)};
}

void
add(std::set<std::size_t> &to, std::set<std::size_t> const &pages) {
    to.insert(pages.begin(), pages.end());
}

TEST(MpscBlockRing, AWindowsMailboxCommitsItsRingsHeadersAndARingsSlotsOnItsProducersFirstWrite) {
    constexpr std::size_t kCores = 64;
    qb::allocator::region_cache::trim(); // a fresh reservation: no block an earlier case left committed pages in

    std::set<std::size_t> written;
    void const           *block = nullptr;
    {
        Mailbox mailbox(kCores, 0ns, 0us);
        ASSERT_TRUE(mailbox.closed());
        std::set<std::size_t> expected;
        for (std::size_t i = 0; i < kCores; ++i)
            add(expected, ring_pages(mailbox, i).header);
        EXPECT_EQ(committed_pages(mailbox.block(), mailbox.block_bytes()), expected)
            << "a mailbox at rest commits its rings' header pages and nothing else";
        if constexpr (sizeof(EventBucket) == 64 && QB_LOCKFREE_CACHELINE_BYTES == 64)
            EXPECT_EQ(expected.size(), kCores + kCores / 8) << "a page per ring, two for the one ring in eight whose 640-byte header "
                                                               "straddles a page";

        EventBucket const bucket{};
        ASSERT_TRUE(mailbox.enqueue(5, bucket));
        add(expected, ring_pages(mailbox, 5).slots);
        EXPECT_EQ(committed_pages(mailbox.block(), mailbox.block_bytes()), expected)
            << "producer 5's first write committed something other than ring 5's slots";

        EXPECT_GE(mailbox.ringOf(7).write_room(3), 3u); // send_run's path: the ring it takes, then the room it asks for
        add(expected, ring_pages(mailbox, 7).slots);
        EXPECT_EQ(committed_pages(mailbox.block(), mailbox.block_bytes()), expected) << "write_room through ringOf";

        EventBucket const run[2]{};
        ASSERT_EQ(mailbox.enqueue(9, run, 2), 2u);
        add(expected, ring_pages(mailbox, 9).slots);
        EXPECT_EQ(committed_pages(mailbox.block(), mailbox.block_bytes()), expected) << "the bulk form";

        std::vector<EventBucket> scratch(64);
        std::size_t              delivered = 0;
        mailbox.consume_all([&delivered](EventBucket *, std::size_t const count) { delivered += count; }, scratch.data(), scratch.size());
        EXPECT_EQ(delivered, 3u);
        EXPECT_EQ(committed_pages(mailbox.block(), mailbox.block_bytes()), expected) << "the consumer's walk committed pages";
        written = expected;
        block   = mailbox.block();
    }

    // The next mailbox of this size is built in the block this one left, what was committed in it kept: it maps nothing,
    // and its rings are closed and empty again.
    std::size_t const mappings = qb::allocator::detail::region_stats().mappings;
    Mailbox           next(kCores, 0ns, 0us);
    EXPECT_EQ(next.block(), block) << "the next mailbox did not take the block the last one gave back";
    EXPECT_EQ(qb::allocator::detail::region_stats().mappings, mappings);
    EXPECT_EQ(committed_pages(next.block(), next.block_bytes()), written);
    EXPECT_FALSE(next.has_data()) << "a ring of the next mailbox showed what the last one's held";
    EXPECT_EQ(next.ringOf(5).producer_seen_read_index(), 1u) << "ring 5 of the next mailbox was not built closed";
    EventBucket const bucket{};
    ASSERT_TRUE(next.enqueue(5, bucket)); // its slots are committed already: opening it commits nothing more
    EXPECT_EQ(committed_pages(next.block(), next.block_bytes()), written);
    std::vector<EventBucket> scratch(64);
    std::size_t              delivered = 0;
    next.consume_all([&delivered](EventBucket *, std::size_t const count) { delivered += count; }, scratch.data(), scratch.size());
    EXPECT_EQ(delivered, 1u) << "ring 5 of the next mailbox delivered what the last one's held";
}

#endif // _WIN32

} // namespace mpsc_block_ring_test
