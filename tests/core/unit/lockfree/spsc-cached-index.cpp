/**
 * @file unit/lockfree/spsc-cached-index.cpp
 * @brief The SPSC ring's two-line layout and its per-side index snapshots.
 *
 * `spsc::internal::ringbuffer` gives each side ONE cache line: the index it publishes and a
 * private snapshot of the peer's index. The fast path decides "not full" / "not empty" from
 * the snapshot and re-reads the peer's line — the only cross-core `acquire` load the ring
 * performs — solely when the snapshot cannot prove it. Measured on a two-core ping-pong the
 * producer-side snapshot alone removed 17 % (Linux) to 28 % (Windows) of the round trip.
 *
 * What this file pins, and why each matters:
 *   - the LAYOUT: exactly two lines, cache-line aligned, so the derived storage starts on a
 *     third line instead of sharing `read_index_`'s (the ring's first slots did until now, and
 *     the producer writes them on every wrap while the consumer writes its index);
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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/spsc.h>

namespace {

constexpr std::size_t kLine = QB_LOCKFREE_CACHELINE_BYTES;
constexpr std::size_t kCap  = 8; // usable capacity; storage is kCap + 1 slots
using Ring                  = qb::lockfree::spsc::ringbuffer<int, kCap>;
using DynRing               = qb::lockfree::spsc::ringbuffer<int, 0>;

// ---- layout ---------------------------------------------------------------------------------

TEST(SpscCachedIndex, BaseIsExactlyTwoAlignedCacheLines) {
    using Base = qb::lockfree::spsc::internal::ringbuffer<int>;
    static_assert(sizeof(Base) == 2 * kLine);
    static_assert(alignof(Base) == kLine);
    static_assert(alignof(Ring) == kLine);
    static_assert(alignof(DynRing) == kLine);
    // The derived storage cannot start before the two index lines end.
    static_assert(sizeof(Ring) >= 2 * kLine + (kCap + 1) * sizeof(int));
    SUCCEED();
}

TEST(SpscCachedIndex, HeapInstancesHonourTheAlignment) {
    auto fixed = std::make_unique<Ring>();
    auto dyn   = std::make_unique<DynRing>(kCap);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(fixed.get()) % kLine, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(dyn.get()) % kLine, 0u);
    std::vector<Ring> v(3); // over-aligned element type through the default allocator
    for (auto &r : v)
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&r) % kLine, 0u);
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
