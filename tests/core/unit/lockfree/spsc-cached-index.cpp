/**
 * @file unit/lockfree/spsc-cached-index.cpp
 * @brief The SPSC ring's four-block layout and its per-side index snapshots.
 *
 * `spsc::internal::ringbuffer` gives each side TWO lines, each in its own two-line block: a
 * private line holding the working index and a snapshot of the peer's index, and a published
 * line holding nothing but the index the peer reads. The fast path decides "not full" / "not
 * empty" from the snapshot and re-reads the peer's published line — the only cross-core
 * `acquire` load the ring performs — solely when the snapshot cannot prove it. Measured on a
 * two-core ping-pong the producer-side snapshot alone removed 17 % (Linux) to 28 % (Windows)
 * of the round trip; keeping the working index OFF the published line (so a publisher never
 * reads the line its peer polls) took qb's two-core ping-pong from 212 to 165 ns and the
 * cross-core ring hop from 115 to 80 ns (QB-184).
 *
 * What this file pins, and why each matters:
 *   - the LAYOUT: exactly eight lines in four two-line blocks, block-aligned, so the derived
 *     storage starts on a fifth block instead of sharing `read_index_`'s line (the ring's first
 *     slots did until 3.0, and the producer writes them on every wrap while the consumer writes
 *     its index);
 *   - a snapshot is a SAFE under-estimate: a stale one can only refuse room / elements that do
 *     exist, never grant ones that do not — every refusal below is followed by the refresh that
 *     must then succeed, in both directions and through both the single and the bulk paths;
 *   - the counts the bulk paths return are the ones the plain implementation returned: a
 *     partial `enqueue<false>` refreshes when the snapshot cannot cover the request, so it
 *     fills every slot that is really free rather than what it last saw;
 *   - a producer and a consumer on two threads, wrapping the storage thousands of times through
 *     mixed single/bulk/consume_all traffic, see every element once and in order.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/spsc.h>

namespace {

constexpr std::size_t kLine  = QB_LOCKFREE_CACHELINE_BYTES;
constexpr std::size_t kBlock = 2 * kLine; // a line and its spatial-prefetch partner
constexpr std::size_t kCap   = 8;         // usable capacity; storage is kCap + 1 slots
using Ring                   = qb::lockfree::spsc::ringbuffer<int, kCap>;
using DynRing                = qb::lockfree::spsc::ringbuffer<int, 0>;

// ---- layout ---------------------------------------------------------------------------------

TEST(SpscCachedIndex, BaseIsExactlyFourAlignedBlocks) {
    using Base = qb::lockfree::spsc::internal::ringbuffer<int>;
    static_assert(sizeof(Base) == 4 * kBlock);
    static_assert(alignof(Base) == kBlock);
    static_assert(alignof(Ring) == kBlock);
    static_assert(alignof(DynRing) == kBlock);
    // The derived storage cannot start before the four index blocks end.
    static_assert(sizeof(Ring) >= 4 * kBlock + (kCap + 1) * sizeof(int));
    SUCCEED();
}

TEST(SpscCachedIndex, HeapInstancesHonourTheAlignment) {
    auto fixed = std::make_unique<Ring>();
    auto dyn   = std::make_unique<DynRing>(kCap);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(fixed.get()) % kBlock, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(dyn.get()) % kBlock, 0u);
    std::vector<Ring> v(3); // over-aligned element type through the default allocator
    for (auto &r : v)
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&r) % kBlock, 0u);
}

// ---- producer snapshot ----------------------------------------------------------------------

TEST(SpscCachedIndex, SingleEnqueueRefusesOnlyWhenReallyFullAndRecoversAfterRefresh) {
    Ring ring;
    for (int i = 0; i < static_cast<int>(kCap); ++i)
        ASSERT_TRUE(ring.enqueue(i)) << "slot " << i;
    EXPECT_FALSE(ring.enqueue(99)) << "capacity is kCap";

    int out = -1;
    ASSERT_TRUE(ring.dequeue(&out));
    EXPECT_EQ(out, 0);
    // The producer's snapshot still says full; the refresh must see the freed slot.
    EXPECT_TRUE(ring.enqueue(100));
    EXPECT_FALSE(ring.enqueue(101));
}

TEST(SpscCachedIndex, BulkAllOrNothingRefreshesBeforeRefusing) {
    Ring             ring;
    std::vector<int> five{1, 2, 3, 4, 5};
    std::vector<int> four{6, 7, 8, 9};
    ASSERT_EQ(ring.enqueue(five.data(), five.size()), five.size());
    EXPECT_EQ(ring.enqueue(four.data(), four.size()), 0u) << "3 free, 4 wanted";

    int out[2];
    ASSERT_EQ(ring.dequeue(out, 2), 2u);
    // Snapshot says 3 free (stale); the request for 4 must force a refresh and then succeed.
    EXPECT_EQ(ring.enqueue(four.data(), four.size()), four.size());
    EXPECT_EQ(ring.enqueue(four.data(), four.size()), 0u) << "1 free, 4 wanted";
}

TEST(SpscCachedIndex, BulkPartialFillsEveryReallyFreeSlot) {
    Ring             ring;
    std::vector<int> five{1, 2, 3, 4, 5};
    std::vector<int> six{6, 7, 8, 9, 10, 11};
    ASSERT_EQ(ring.enqueue(five.data(), five.size()), five.size());
    int out[2];
    ASSERT_EQ(ring.dequeue(out, 2), 2u);
    // 5 slots are really free; a snapshot that only ever saw 3 must not answer 3.
    EXPECT_EQ(ring.enqueue<false>(six.data(), six.size()), 5u);
    EXPECT_EQ(ring.enqueue<false>(six.data(), six.size()), 0u);
    // The consumer's snapshot last saw 5 published (3 unread); asking for 8 must refresh.
    std::vector<int> drained(kCap, 0);
    ASSERT_EQ(ring.dequeue(drained.data(), drained.size()), kCap);
    EXPECT_EQ(drained, (std::vector<int>{3, 4, 5, 6, 7, 8, 9, 10}));
}

TEST(SpscCachedIndex, ChunkedDequeueRefreshesOnlyWhenTheChunkOutgrowsTheSnapshot) {
    Ring             ring;
    std::vector<int> six{1, 2, 3, 4, 5, 6};
    ASSERT_EQ(ring.enqueue(six.data(), six.size()), six.size());
    int out[4];
    ASSERT_EQ(ring.dequeue(out, 4), 4u); // refresh: sees 6
    ASSERT_TRUE(ring.enqueue(7));        // published after that snapshot
    ASSERT_TRUE(ring.enqueue(8));
    // 2 left in the snapshot, chunk of 2: served from the snapshot alone — correct, just older.
    ASSERT_EQ(ring.dequeue(out, 2), 2u);
    EXPECT_EQ(out[0], 5);
    EXPECT_EQ(out[1], 6);
    // Snapshot now says empty; the chunk forces the refresh that finds 7 and 8.
    ASSERT_EQ(ring.dequeue(out, 4), 2u);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[1], 8);
    EXPECT_FALSE(ring.dequeue(out));
}

// ---- consumer snapshot ----------------------------------------------------------------------

TEST(SpscCachedIndex, DequeueSeesEveryPublishedElementAndRefreshesOnEmpty) {
    Ring ring;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(ring.enqueue(i));
    int out = -1;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(ring.dequeue(&out)) << "element " << i;
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.dequeue(&out)) << "empty after three";
    // The consumer's snapshot says empty; a fresh element must be found by the refresh.
    ASSERT_TRUE(ring.enqueue(42));
    ASSERT_TRUE(ring.dequeue(&out));
    EXPECT_EQ(out, 42);
}

TEST(SpscCachedIndex, ConsumeAllDrainsWhatWasPublishedAfterItsLastSnapshot) {
    // Value-initialised: the slots are indeterminate by design (a producer writes before a consumer
    // reads), and gcc-14 cannot see that ordering through the consume_all lambda below.
    Ring ring{};
    auto drain = [&] {
        std::vector<int> seen;
        ring.consume_all([&](int *p, std::size_t n) { seen.insert(seen.end(), p, p + n); });
        return seen;
    };
    EXPECT_TRUE(drain().empty());
    ASSERT_TRUE(ring.enqueue(1));
    ASSERT_TRUE(ring.enqueue(2));
    EXPECT_EQ(drain(), (std::vector<int>{1, 2}));
    std::vector<int> three{3, 4, 5};
    ASSERT_EQ(ring.enqueue(three.data(), three.size()), 3u);
    EXPECT_EQ(drain(), three);
    EXPECT_TRUE(drain().empty());
}

TEST(SpscCachedIndex, PublicQueriesNeverUseASnapshot) {
    Ring ring;
    EXPECT_TRUE(ring.empty());
    ASSERT_TRUE(ring.enqueue(7));
    EXPECT_FALSE(ring.empty()); // neither side has refreshed anything: the query must not need it
    int out;
    ASSERT_TRUE(ring.dequeue(&out));
    EXPECT_TRUE(ring.empty());
}

// ---- write_room: the producer-side room query behind a bulk run publish ------------------------
//
// `SharedCoreCommunication::send_run` asks the ring once how many slots it can take RIGHT NOW,
// cuts its run of whole events at that count, and then relies on `enqueue<true>` of at most
// that many slots never refusing. Both halves are pinned here: the count is exact after a
// refresh (never an over-estimate, never a stale under-estimate when the snapshot cannot prove
// the request), and the all-or-nothing enqueue that follows it succeeds.

TEST(SpscCachedIndex, WriteRoomReportsTheExactFreeCountAndAnEnqueueOfItCannotFail) {
    Ring ring;
    EXPECT_EQ(ring.write_room(kCap), kCap);
    EXPECT_EQ(ring.write_room(kCap + 5), kCap) << "capped at the capacity, never above it";
    EXPECT_EQ(ring.write_room(3), kCap) << "the answer is the room, not the request";

    const int five[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(ring.enqueue(five, 5), 5u);
    EXPECT_EQ(ring.write_room(kCap), kCap - 5);

    // The consumer frees two slots the producer snapshot cannot see: asking for more than the
    // snapshot proves refreshes, and the answer is the REAL room.
    int out[8];
    ASSERT_EQ(ring.dequeue(out, 2), 2u);
    const std::size_t room = ring.write_room(kCap);
    EXPECT_EQ(room, kCap - 3);

    // What write_room promised, enqueue<true> takes whole.
    const int fill[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    EXPECT_EQ(ring.enqueue(fill, room), room);
    EXPECT_EQ(ring.write_room(1), 0u) << "full";
    EXPECT_EQ(ring.enqueue(fill, 1), 0u);

    // The order survives the run cut: 3,4,5 then the fill.
    ASSERT_EQ(ring.dequeue(out, 8), 8u);
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[2], 5);
    EXPECT_EQ(out[3], 10);
    EXPECT_EQ(out[7], 14);
}

TEST(SpscCachedIndex, WriteRoomIsASafeUnderEstimateUntilAskedForMore) {
    Ring      ring;
    const int five[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(ring.enqueue(five, 5), 5u);
    ASSERT_EQ(ring.write_room(kCap), kCap - 5); // snapshot: 3 free

    int out[8];
    ASSERT_EQ(ring.dequeue(out, 5), 5u); // now 8 free, snapshot still says 3

    // A request the snapshot covers is answered from it — correct, and an under-estimate that
    // can only refuse room, never grant room that does not exist.
    EXPECT_EQ(ring.write_room(2), 3u);
    EXPECT_EQ(ring.write_room(3), 3u);
    // One it cannot cover forces the refresh and sees all eight.
    EXPECT_EQ(ring.write_room(4), kCap);
}

TEST(SpscCachedIndex, WriteRoomOnTheDynamicRingHonoursItsRuntimeCapacity) {
    DynRing ring(5);
    EXPECT_EQ(ring.write_room(100), 5u);
    const int three[3] = {1, 2, 3};
    ASSERT_EQ(ring.enqueue(three, 3), 3u);
    EXPECT_EQ(ring.write_room(100), 2u);
    EXPECT_EQ(ring.enqueue(three, 2), 2u);
    EXPECT_EQ(ring.write_room(1), 0u);
}

// ---- synchronous producer transactions -------------------------------------------------------

TEST(SpscCachedIndex, EnqueueWithDoesNotCallTheFillerForZeroOrAnInsufficientWrite) {
    Ring        ring{};
    std::size_t calls = 0;
    int         next  = 100;
    auto const  fill  = [&](int *first, std::size_t first_count, int *second, std::size_t second_count) noexcept {
        ++calls;
        for (std::size_t i = 0; i < first_count; ++i)
            first[i] = next++;
        for (std::size_t i = 0; i < second_count; ++i)
            second[i] = next++;
    };

    EXPECT_EQ(ring.enqueue_with(0, fill), 0u);
    EXPECT_EQ(ring.enqueue_with(kCap + 1, fill), 0u);
    EXPECT_EQ(calls, 0u);
    EXPECT_TRUE(ring.empty());
    ASSERT_EQ(ring.enqueue_with(kCap, fill), kCap);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(ring.enqueue_with(1, fill), 0u) << "full";
    EXPECT_EQ(ring.enqueue_with(0, fill), 0u);
    EXPECT_EQ(calls, 1u);

    int out[2];
    ASSERT_EQ(ring.dequeue(out, 2), 2u);
    EXPECT_EQ(out[0], 100);
    EXPECT_EQ(out[1], 101);
    EXPECT_EQ(ring.enqueue_with(3, fill), 0u) << "two slots free: no partial callback";
    EXPECT_EQ(calls, 1u);
    ASSERT_EQ(ring.enqueue_with(2, fill), 2u) << "refresh must see the two freed slots";
    EXPECT_EQ(calls, 2u);
    std::vector<int> drained(kCap);
    ASSERT_EQ(ring.dequeue(drained.data(), drained.size()), kCap);
    EXPECT_EQ(drained, (std::vector<int>{102, 103, 104, 105, 106, 107, 108, 109}));
    EXPECT_TRUE(ring.empty());
}

TEST(SpscCachedIndex, EnqueueWithWrapPreservesUnreadItemsAndPresentsBothRunsInOrder) {
    Ring      ring{};
    const int prefix[7] = {1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ring.enqueue(prefix, 7), 7u);
    int removed[5];
    ASSERT_EQ(ring.dequeue(removed, 5), 5u);

    std::size_t calls = 0, first_size = 0, second_size = 0;
    ASSERT_EQ(ring.enqueue_with(4,
                                [&](int *first, std::size_t first_count, int *second, std::size_t second_count) noexcept {
                                    ++calls;
                                    first_size  = first_count;
                                    second_size = second_count;
                                    for (std::size_t i = 0; i < first_count; ++i)
                                        first[i] = 8 + static_cast<int>(i);
                                    for (std::size_t i = 0; i < second_count; ++i)
                                        second[i] = 8 + static_cast<int>(first_count + i);
                                }),
              4u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(first_size, 2u) << "the storage has nine slots and the writer is at slot seven";
    EXPECT_EQ(second_size, 2u);
    std::vector<int> drained(6);
    ASSERT_EQ(ring.dequeue(drained.data(), drained.size()), drained.size());
    EXPECT_EQ(drained, (std::vector<int>{6, 7, 8, 9, 10, 11}));
    EXPECT_TRUE(ring.empty());
}

TEST(SpscCachedIndex, EnqueueWithExactEndStartsTheNextWriteAtTheFirstSlot) {
    Ring      ring{};
    const int prefix[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(ring.enqueue(prefix, 5), 5u);
    int removed[5];
    ASSERT_EQ(ring.dequeue(removed, 5), 5u);

    std::size_t first_size = 0, second_size = 0;
    int         next = 10;
    auto const  fill = [&](int *first, std::size_t first_count, int *second, std::size_t second_count) noexcept {
        first_size  = first_count;
        second_size = second_count;
        for (std::size_t i = 0; i < first_count; ++i)
            first[i] = next++;
        for (std::size_t i = 0; i < second_count; ++i)
            second[i] = next++;
    };
    ASSERT_EQ(ring.enqueue_with(4, fill), 4u);
    EXPECT_EQ(first_size, 4u);
    EXPECT_EQ(second_size, 0u) << "exact end is one run, not an extra slot";
    ASSERT_EQ(ring.enqueue_with(2, fill), 2u);
    EXPECT_EQ(first_size, 2u);
    EXPECT_EQ(second_size, 0u);
    std::vector<int> drained(6);
    ASSERT_EQ(ring.dequeue(drained.data(), drained.size()), drained.size());
    EXPECT_EQ(drained, (std::vector<int>{10, 11, 12, 13, 14, 15}));
    EXPECT_TRUE(ring.empty());
}

TEST(SpscCachedIndex, EnqueueWithDoesNotPublishTheFirstRunBeforeTheCallbackFinishesTheSecond) {
    Ring      ring{};
    const int prefix[7] = {1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(ring.enqueue(prefix, 7), 7u);
    int removed[7];
    ASSERT_EQ(ring.dequeue(removed, 7), 7u); // the next four slots wrap as two plus two

    std::atomic<bool> entered{false}, finish{false}, done{false};
    std::size_t       published = 0, first_size = 0, second_size = 0;
    std::thread       producer([&] {
        published = ring.enqueue_with(4, [&](int *first, std::size_t first_count, int *second, std::size_t second_count) noexcept {
            first_size  = first_count;
            second_size = second_count;
            for (std::size_t i = 0; i < first_count; ++i)
                first[i] = 100 + static_cast<int>(i);
            entered.store(true, std::memory_order_release);
            while (!finish.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (std::size_t i = 0; i < second_count; ++i)
                second[i] = 100 + static_cast<int>(first_count + i);
        });
        done.store(true, std::memory_order_release);
    });

    // No fatal assertion until the producer is released and joined: a refused transaction
    // or a missing callback must fail the test rather than leave a joinable thread behind.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!entered.load(std::memory_order_acquire) && !done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_TRUE(entered.load(std::memory_order_acquire));
    EXPECT_TRUE(ring.empty()) << "even the filled first run is unpublished while the callback is pending";
    finish.store(true, std::memory_order_release);
    producer.join();

    ASSERT_EQ(published, 4u);
    EXPECT_EQ(first_size, 2u);
    EXPECT_EQ(second_size, 2u);
    std::vector<int> drained(4);
    ASSERT_EQ(ring.dequeue(drained.data(), drained.size()), drained.size());
    EXPECT_EQ(drained, (std::vector<int>{100, 101, 102, 103}));
    EXPECT_TRUE(ring.empty());
}

// ---- two threads, thousands of wraps ------------------------------------------------------------

TEST(SpscCachedIndex, TwoThreadsMixedTrafficDeliverEveryElementOnceInOrder) {
    constexpr std::size_t                       kSlots = 64;
    constexpr int                               kTotal = 400000; // 6250 wraps of the storage
    qb::lockfree::spsc::ringbuffer<int, kSlots> ring;

    std::thread producer([&] {
        int next = 0;
        int batch[7];
        while (next < kTotal) {
            if ((next & 3) == 0) { // bulk, all-or-nothing, 1..7 elements
                const std::size_t n = static_cast<std::size_t>(1 + (next % 7));
                if (next + static_cast<int>(n) > kTotal)
                    break;
                for (std::size_t i = 0; i < n; ++i)
                    batch[i] = next + static_cast<int>(i);
                if (ring.enqueue(batch, n) == n)
                    next += static_cast<int>(n);
            } else {
                if (ring.enqueue(next))
                    ++next;
            }
        }
        // finish the tail one element at a time (the bulk branch may have refused to overshoot)
        while (next < kTotal)
            if (ring.enqueue(next))
                ++next;
    });

    int  expected = 0;
    bool ordered  = true;
    int  scratch[5];
    while (expected < kTotal) {
        if ((expected & 1) == 0) {
            ring.consume_all([&](int *p, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i)
                    ordered &= (p[i] == expected++);
            });
        } else {
            const std::size_t n = ring.dequeue(scratch, 5);
            for (std::size_t i = 0; i < n; ++i)
                ordered &= (scratch[i] == expected++);
        }
        if (!ordered)
            break;
    }
    producer.join();
    EXPECT_TRUE(ordered);
    EXPECT_EQ(expected, kTotal);
    EXPECT_TRUE(ring.empty());
}

} // namespace
