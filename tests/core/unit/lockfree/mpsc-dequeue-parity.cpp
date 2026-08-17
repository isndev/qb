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
 * @brief Pins what `size` means in EACH of the two same-named `mpsc::ringbuffer::dequeue`
 *        overloads, and that they deliberately disagree.
 *
 * `dequeue(T *ret, size_t size)` and `dequeue(Func const &, T *ret, size_t size)` share a
 * name and a parameter list shape, and `size` means something different in each:
 *
 *   - array overload  — `ret` is the OUTPUT, producers append into it, so `size` is a
 *                       **total** budget and the return is bounded by it;
 *   - functor overload — `ret` is SCRATCH, rewritten from index 0 once per producer, so
 *                       `size` is that buffer's capacity and therefore a **per-producer**
 *                       chunk limit; the return is a total and can reach `nb_producer * size`.
 *
 * That looks like a bug from the outside and was reported as one. It is not: the engine
 * depends on the per-producer form. `VirtualCore::__receive__` passes the scratch capacity
 * (`MaxRingEvents`, exactly one ring's readable maximum) precisely so that EVERY core's ring
 * is drained on every turn. Carrying a budget across the producer loop would let the first
 * saturated producer consume it and starve every other core — a fairness regression on the
 * event path, under exactly the load that makes it matter.
 *
 * So the missing test was never "assert the functor overload honours a budget". It was:
 * **assert the two overloads against each other**, so the divergence is a recorded decision
 * with a reason rather than an accident nobody compared. A future "consistency" cleanup that
 * makes the functor overload honour a total budget fails `FunctorOverloadIsPerProducer` and
 * `FunctorOverloadDrainsEveryProducer` — the second of which is the engine invariant.
 */

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/mpsc.h>

namespace mpsc_dequeue_parity_test {

constexpr std::size_t kProducers = 3;
constexpr std::size_t kCapacity  = 64;
constexpr std::size_t kPerRing   = 20;
constexpr std::size_t kBudget    = 8;

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
// The array overload: `size` is a TOTAL budget
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
// The functor overload: `size` is a PER-PRODUCER chunk limit
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, FunctorOverloadIsPerProducer) {
    Ring ring;
    fill(ring);

    std::vector<int> scratch(kBudget);
    std::size_t      seen     = 0;
    std::size_t      batches  = 0;
    std::size_t      widest   = 0;
    const auto       consumed = ring.dequeue(
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

TEST(MpscDequeueParity, FunctorOverloadNeverOverrunsTheScratchBuffer) {
    // The divergence is only safe because `ret` is scratch: no batch may exceed its
    // capacity even though the total does. Pinned explicitly, because "the return exceeds
    // size" would be a buffer overflow if `ret` were an output array.
    Ring ring;
    fill(ring);

    std::vector<int> scratch(kBudget);
    bool             overrun = false;
    ring.dequeue([&](int *, std::size_t count) { overrun = overrun || (count > kBudget); }, scratch.data(), kBudget);

    EXPECT_FALSE(overrun);
}

TEST(MpscDequeueParity, FunctorOverloadDrainsEveryProducer) {
    // THE ENGINE INVARIANT. `VirtualCore::__receive__` passes the scratch capacity, which
    // is one ring's readable maximum, and relies on every ring being drained in that one
    // call. If a future change carries a budget across the producer loop, this fails: the
    // first ring would eat the budget and the rest would go unread — starvation.
    Ring ring;
    fill(ring);

    std::vector<int> scratch(kCapacity);
    std::vector<int> all;
    const auto       consumed =
        ring.dequeue([&](int *batch, std::size_t count) { all.insert(all.end(), batch, batch + count); }, scratch.data(), kCapacity);

    EXPECT_EQ(consumed, kPerRing * kProducers) << "one call must drain every producer ring";
    ASSERT_EQ(all.size(), kPerRing * kProducers);
    // Every producer is represented, in producer order.
    EXPECT_EQ(all.front(), 0);
    EXPECT_EQ(all[kPerRing], 1000);
    EXPECT_EQ(all[kPerRing * 2], 2000);
}

// ---------------------------------------------------------------------------
// The pair, asserted against each other
// ---------------------------------------------------------------------------

TEST(MpscDequeueParity, TheTwoOverloadsAgreeOnContentAndDisagreeOnBudget) {
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
        const auto       n_functor = single_b.dequeue(
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
    const auto       n_functor = functor_ring.dequeue([](int *, std::size_t) {}, scratch.data(), kBudget);

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
    EXPECT_EQ(ring2.dequeue([](int *, std::size_t) {}, scratch.data(), kBudget), kBudget * kProducers)
        << "functor overload: per-producer chunk";
}

} // namespace mpsc_dequeue_parity_test
