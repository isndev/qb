/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/lockfree/mpsc-dequeue-parity.cpp
 * @brief Pins the two drain contracts of `mpsc::ringbuffer` — one bounded, one not — against
 *        each other, and against the spsc primitive whose name they used to borrow.
 *
 * Two operations, deliberately different, and since 3.0 differently named:
 *
 *   - `dequeue(T *ret, size_t size)`            — `ret` is the OUTPUT, producers append into
 *                                                 it, so `size` is a **total** budget and the
 *                                                 return is bounded by it;
 *   - `consume_all(Func const &, T *scratch, size_t chunk)`
 *                                               — `scratch` is rewritten from index 0 once
 *                                                 per producer, so `chunk` is a **per-producer**
 *                                                 batch limit; the return is a total and can
 *                                                 reach `nb_producer * chunk`.
 *
 * The second was called `dequeue(Func, ret, size)` until 3.0, and that name was the defect —
 * not the behaviour. It made an unbounded drain read as a bounded sibling of the overload
 * above, and, worse, the SAME name one layer down genuinely is bounded:
 * `spsc::ringbuffer::dequeue(func, ret, size)` copies out at most `size` and calls the functor
 * once. So the mpsc loop silently changed the meaning of the argument it forwarded, and a
 * reader who had learned the spsc primitive — which is exactly what `ringOf(i)` hands them —
 * was misled by the layer above. `SpscFunctorDequeueIsBoundedUnlikeTheMpscLoop` is that
 * comparison, and it is the assertion no per-class test could have made.
 *
 * The per-producer budget itself is load-bearing and must not be "fixed": the engine depends
 * on it. `VirtualCore::__receive__` passes the scratch capacity (`MaxRingEvents`, exactly one
 * ring's readable maximum) precisely so that EVERY core's ring is drained on every turn.
 * Carrying a budget across the producer loop would let the first saturated producer consume it
 * and starve every other core — a fairness regression on the event path, under exactly the
 * load that makes it matter. A "consistency" cleanup that did it fails
 * `ConsumeAllIsPerProducer` and `ConsumeAllDrainsEveryProducer`, the second being the engine
 * invariant.
 *
 * The old spelling is GONE rather than deprecated: 3.0 reforms the API, and an alias nobody
 * is asked to migrate off is debt that outlives the reason for it. So there is no forwarder
 * to compare against — what protects the rename is that the contracts below are asserted
 * against each other and across the two layers.
 */

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/mpsc.h>
#include <qb/system/lockfree/spsc.h>

namespace mpsc_dequeue_parity_test {

constexpr std::size_t kProducers = 3;
constexpr std::size_t kCapacity  = 64;
constexpr std::size_t kPerRing   = 20;
constexpr std::size_t kBudget    = 8;
/// A ring filled to its usable maximum — one slot is reserved to tell full from empty. Used by
/// the engine-invariant test, which is only sensitive to a shared budget when the FIRST
/// producer alone can exhaust it.
constexpr std::size_t kSaturated = kCapacity - 1;

using Ring = qb::lockfree::mpsc::ringbuffer<int, kCapacity, kProducers>;

/// Fill every producer ring with `kPerRing` items, tagged by producer so we can tell the
/// rings apart in the output.
void
fill(Ring &ring) {
    for (std::size_t p = 0; p < kProducers; ++p)
        for (std::size_t i = 0; i < kPerRing; ++i)
            ASSERT_TRUE(ring.enqueue(p, static_cast<int>(p * 1000 + i)));
}

// ---------------------------------------------------------------------------
// dequeue(T*, size): `size` is a TOTAL budget
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, ArrayOverloadHonoursSizeAsATotalBudget) {
    Ring ring;
    fill(ring);

    std::vector<int> out(kBudget);
    const auto       n = ring.dequeue(out.data(), kBudget);

    EXPECT_EQ(n, kBudget) << "the array overload's `size` is a total budget";
    EXPECT_LE(n, kBudget);
}

TEST(MpscDequeueParity, ArrayOverloadAppendsRatherThanOverwrites) {
    // A budget larger than one ring must draw from more than one producer, and the second
    // producer's items must land AFTER the first's, not on top of them.
    Ring ring;
    fill(ring);

    std::vector<int> out(kPerRing + 5);
    const auto       n = ring.dequeue(out.data(), kPerRing + 5);
    ASSERT_EQ(n, kPerRing + 5);

    EXPECT_EQ(out[0], 0) << "producer 0's first item";
    EXPECT_EQ(out[kPerRing - 1], static_cast<int>(kPerRing - 1)) << "producer 0's last item";
    EXPECT_EQ(out[kPerRing], 1000) << "producer 1 appends after producer 0";
}

// ---------------------------------------------------------------------------
// consume_all(func, scratch, chunk): `chunk` is a PER-PRODUCER batch limit
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, ConsumeAllIsPerProducer) {
    Ring ring;
    fill(ring);

    std::vector<int> scratch(kBudget);
    std::size_t      seen     = 0;
    std::size_t      batches  = 0;
    std::size_t      widest   = 0;
    const auto       consumed = ring.consume_all(
        [&](int *, std::size_t count) {
            ++batches;
            seen += count;
            widest = (count > widest) ? count : widest;
        },
        scratch.data(), kBudget);

    // The reported "defect": 3 producers x 20 items with a budget of 8 yields 24, not 8.
    // That is the contract, not a leak — one chunk of at most `kBudget` per producer.
    EXPECT_EQ(consumed, kBudget * kProducers) << "`size` bounds each producer's chunk, not the total";
    EXPECT_EQ(seen, consumed);
    EXPECT_EQ(batches, kProducers) << "one functor call per non-empty producer ring";
    EXPECT_LE(widest, kBudget) << "no single batch may exceed the scratch buffer's capacity";
    EXPECT_GT(consumed, kBudget) << "the total deliberately exceeds `size` — this is the divergence";
}

TEST(MpscDequeueParity, ConsumeAllNeverOverrunsTheScratchBuffer) {
    // The divergence is only safe because `ret` is scratch: no batch may exceed its
    // capacity even though the total does. Pinned explicitly, because "the return exceeds
    // size" would be a buffer overflow if `ret` were an output array.
    Ring ring;
    fill(ring);

    std::vector<int> scratch(kBudget);
    bool             overrun = false;
    ring.consume_all([&](int *, std::size_t count) { overrun = overrun || (count > kBudget); }, scratch.data(), kBudget);

    EXPECT_FALSE(overrun);
}

TEST(MpscDequeueParity, ConsumeAllDrainsEveryProducer) {
    // THE ENGINE INVARIANT. `VirtualCore::__receive__` passes the scratch capacity — one
    // ring's readable maximum — and relies on every ring being drained in that one call. A
    // future change that carries a budget across the producer loop must fail HERE.
    //
    // The rings are filled to SATURATION deliberately. An earlier version of this test used
    // 20 items per ring against a chunk of 64 and passed under a planted shared budget, since
    // the budget only bites once the first producer alone can exhaust it — so the test that
    // claimed to guard the invariant did not detect the starvation it was written for. The
    // engine's own case is the saturated one: `MaxRingEvents` IS one ring's capacity, so a
    // single busy peer core fills it exactly.
    Ring ring;
    for (std::size_t p = 0; p < kProducers; ++p)
        for (std::size_t i = 0; i < kSaturated; ++i)
            ASSERT_TRUE(ring.enqueue(p, static_cast<int>(p * 1000 + i))) << "producer " << p << " ring full at " << i;

    std::vector<int> scratch(kSaturated);
    std::vector<int> all;
    std::size_t      batches  = 0;
    const auto       consumed = ring.consume_all(
        [&](int *batch, std::size_t count) {
            ++batches;
            all.insert(all.end(), batch, batch + count);
        },
        scratch.data(), kSaturated);

    EXPECT_EQ(consumed, kSaturated * kProducers) << "one call must drain every producer ring, even when the first alone could "
                                                    "exhaust a shared budget — that is the starvation this pins";
    EXPECT_EQ(batches, kProducers) << "every producer must be visited; a shared budget would stop after the first";
    ASSERT_EQ(all.size(), kSaturated * kProducers);
    // Every producer is represented, in producer order.
    EXPECT_EQ(all.front(), 0);
    EXPECT_EQ(all[kSaturated], 1000);
    EXPECT_EQ(all[kSaturated * 2], 2000);
}

// ---------------------------------------------------------------------------
// The pair, asserted against each other
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, TheTwoDrainsAgreeOnContentAndDisagreeOnBudget) {
    // Same data, same `size`, two overloads. They must return the SAME items in the same
    // order when the budget cannot bite (one producer, budget >= its contents), and must
    // differ exactly when it can (several producers).
    {
        qb::lockfree::mpsc::ringbuffer<int, kCapacity, 1> single_a;
        qb::lockfree::mpsc::ringbuffer<int, kCapacity, 1> single_b;
        for (std::size_t i = 0; i < 5; ++i) {
            ASSERT_TRUE(single_a.enqueue(0, static_cast<int>(i)));
            ASSERT_TRUE(single_b.enqueue(0, static_cast<int>(i)));
        }

        std::vector<int> via_array(kBudget);
        const auto       n_array = single_a.dequeue(via_array.data(), kBudget);
        via_array.resize(n_array);

        std::vector<int> scratch(kBudget);
        std::vector<int> via_functor;
        const auto       n_functor = single_b.consume_all(
            [&](int *batch, std::size_t count) { via_functor.insert(via_functor.end(), batch, batch + count); }, scratch.data(), kBudget);

        EXPECT_EQ(n_array, n_functor) << "with one producer the two overloads must agree exactly";
        EXPECT_EQ(via_array, via_functor);
    }

    // With several producers they diverge, and the divergence is the documented one.
    Ring array_ring;
    Ring functor_ring;
    fill(array_ring);
    fill(functor_ring);

    std::vector<int> out(kBudget);
    const auto       n_array = array_ring.dequeue(out.data(), kBudget);

    std::vector<int> scratch(kBudget);
    const auto       n_functor = functor_ring.consume_all([](int *, std::size_t) {}, scratch.data(), kBudget);

    EXPECT_EQ(n_array, kBudget);
    EXPECT_EQ(n_functor, kBudget * kProducers);
    EXPECT_NE(n_array, n_functor) << "the divergence is deliberate; see the header's @warning on both overloads";
}

// ---------------------------------------------------------------------------
// Same two properties on the runtime-producer specialisation, which the engine
// actually instantiates (`Mailbox` is <EventBucket, MaxRingEvents, 0>).
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, RuntimeSpecialisationHasTheSameTwoContracts) {
    qb::lockfree::mpsc::ringbuffer<int, kCapacity, 0> ring(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p)
        for (std::size_t i = 0; i < kPerRing; ++i)
            ASSERT_TRUE(ring.enqueue(p, static_cast<int>(p * 1000 + i)));

    std::vector<int> out(kBudget);
    EXPECT_EQ(ring.dequeue(out.data(), kBudget), kBudget) << "array overload: total budget";

    qb::lockfree::mpsc::ringbuffer<int, kCapacity, 0> ring2(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p)
        for (std::size_t i = 0; i < kPerRing; ++i)
            ASSERT_TRUE(ring2.enqueue(p, static_cast<int>(p * 1000 + i)));

    std::vector<int> scratch(kBudget);
    EXPECT_EQ(ring2.consume_all([](int *, std::size_t) {}, scratch.data(), kBudget), kBudget * kProducers)
        << "functor overload: per-producer chunk";
}

// ---------------------------------------------------------------------------
// Across the layers: the name that was wrong, and why it was wrong
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, SpscFunctorDequeueIsBoundedUnlikeTheMpscLoop) {
    // THE COMPARISON THAT NAMES THE DEFECT. One layer down, `dequeue(func, ret, size)` copies
    // out AT MOST `size` and calls the functor once — genuinely bounded, and a proper sibling
    // of `dequeue(T*, size)`. The mpsc loop forwarded that same call once per producer while
    // keeping the name, so the identical spelling meant "bounded" here and "unbounded" one
    // level up. Nothing compared the layers, so nothing could see it. Since 3.0 the unbounded
    // one is `consume_all`, and this test is what keeps the two honest.
    qb::lockfree::spsc::ringbuffer<int, kCapacity> spsc;
    for (std::size_t i = 0; i < kPerRing; ++i)
        ASSERT_TRUE(spsc.enqueue(static_cast<int>(i)));

    std::vector<int> scratch(kBudget);
    std::size_t      calls = 0;
    std::size_t      seen  = 0;
    const auto       n     = spsc.dequeue(
        [&](int *, std::size_t count) {
            ++calls;
            seen += count;
        },
        scratch.data(), kBudget);

    EXPECT_EQ(n, kBudget) << "the spsc functor dequeue IS bounded by `size` — that is the whole point";
    EXPECT_EQ(calls, 1u) << "one ring, one batch, one call";
    EXPECT_EQ(seen, kBudget);

    // Same data, same third argument, one producer's worth — but through the mpsc drain,
    // whose return is a total over every ring. With three filled rings it is three times as
    // large, and that difference is precisely what the two names now carry.
    Ring mpsc_ring;
    fill(mpsc_ring);
    std::vector<int> mpsc_scratch(kBudget);
    const auto       mpsc_n = mpsc_ring.consume_all([](int *, std::size_t) {}, mpsc_scratch.data(), kBudget);

    EXPECT_EQ(mpsc_n, kBudget * kProducers);
    EXPECT_NE(n, mpsc_n) << "same third argument, same spelling before 3.0, different contract";
}

} // namespace mpsc_dequeue_parity_test
