/**
 * @file unit/lockfree/ring-wrap-batching.cpp
 * @brief The SPSC ring's two batch-consumption primitives and their wrap-around contract.
 *
 * A `qb::Event` occupies a WHOLE NUMBER OF RING SLOTS (`EventBucket`s) and the producer's
 * `enqueue<_All=true>` is atomic but NOT wrap-aligned: it deliberately splits an item across the
 * end of the storage with a two-section memcpy. Any consumer that parses variable-length items by
 * an embedded size field therefore needs its batch delivered CONTIGUOUSLY.
 *
 * Two primitives exist and they differ exactly here:
 *   - `dequeue(func, scratch, n)` COPIES OUT first  -> one contiguous, item-aligned batch;
 *   - `consume_all(func)`         walks IN PLACE    -> TWO calls when the range wraps, which
 *                                                     tears a multi-slot item in half.
 *
 * Regression: `SharedCoreCommunication::dispose_residual_mailbox_events()` used `consume_all`.
 * On a saturated mailbox (guaranteed to wrap — and exactly the state that teardown sweep exists
 * for) a multi-bucket, non-trivially-destructible event straddling the wrap was torn: the first
 * call disposed an event whose payload bytes were not there (running `~std::string` on
 * out-of-range memory) and the second reinterpreted that event's TAIL buckets as a fresh event
 * header and disposed a bogus type. It now uses the copy-out `dequeue`, the same primitive the
 * live receive path uses.
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
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/spsc.h>

namespace {

constexpr std::size_t kSlots = 8;
using Ring                   = qb::lockfree::spsc::ringbuffer<int, kSlots>;

// Advance the read/write indices so the NEXT enqueue of `item_len` straddles the wrap.
void
prime_to_wrap(Ring &ring, std::size_t filler, std::size_t item_len) {
    std::vector<int> in(filler, 0);
    std::vector<int> out(filler, 0);
    ASSERT_EQ(ring.enqueue(in.data(), filler), filler);
    ASSERT_EQ(ring.dequeue(out.data(), filler), filler);
    // storage is kSlots + 1 slots; write index now sits at `filler`
    ASSERT_GT(filler + item_len, kSlots + 1) << "test setup does not actually wrap";
}

} // namespace

// A multi-slot item is enqueued atomically even when it straddles the wrap.
TEST(RingWrapBatching, MultiSlotEnqueueIsAtomicAcrossTheWrap) {
    Ring ring;
    prime_to_wrap(ring, 6, 4);

    const int item[4] = {11, 12, 13, 14};
    EXPECT_EQ(ring.enqueue(item, 4), 4u) << "all-or-nothing enqueue must place the whole item";
}

// The COPY-OUT dequeue reassembles the wrapped item into ONE contiguous batch.
TEST(RingWrapBatching, CopyOutDequeueDeliversAWrappedItemContiguously) {
    Ring ring;
    prime_to_wrap(ring, 6, 4);
    const int item[4] = {11, 12, 13, 14};
    ASSERT_EQ(ring.enqueue(item, 4), 4u);

    std::vector<int>              scratch(kSlots + 1, 0);
    int                           calls = 0;
    std::vector<std::vector<int>> batches;
    ring.dequeue(
        [&](int *buf, std::size_t cnt) {
            ++calls;
            batches.emplace_back(buf, buf + cnt);
        },
        scratch.data(), kSlots + 1);

    ASSERT_EQ(calls, 1) << "a copy-out dequeue must deliver the wrapped item in a single batch";
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0], (std::vector<int>{11, 12, 13, 14})) << "the item must arrive intact and in order";
}

// consume_all walks in place and therefore SPLITS the item — the documented hazard that made it
// the wrong primitive for the residual-mailbox sweep. Pinned so the contract cannot silently flip.
TEST(RingWrapBatching, ConsumeAllSplitsAWrappedItemAcrossTwoCalls) {
    Ring ring;
    prime_to_wrap(ring, 6, 4);
    const int item[4] = {11, 12, 13, 14};
    ASSERT_EQ(ring.enqueue(item, 4), 4u);

    int              calls = 0;
    std::vector<int> seen;
    ring.consume_all([&](int *buf, std::size_t cnt) {
        ++calls;
        seen.insert(seen.end(), buf, buf + cnt);
    });

    EXPECT_EQ(calls, 2) << "consume_all is expected to split at the wrap — this is why a "
                           "size-prefixed consumer must not use it";
    EXPECT_EQ(seen, (std::vector<int>{11, 12, 13, 14})) << "the elements themselves are all delivered, just not contiguously";
}
