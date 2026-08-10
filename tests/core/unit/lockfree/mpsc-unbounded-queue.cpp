/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/lockfree/mpsc-unbounded-queue.cpp
 * @brief `qb::lockfree::mpsc_unbounded_queue` — the shipped unbounded Vyukov MPSC queue.
 *
 * A public `qb/system/lockfree/` header with **no in-tree caller and no test**: nothing in qb, qbm
 * or the examples instantiates it, so every one of its guarantees was unverified while still being
 * part of the released API surface ("suitable for coroutine ready-queues, task queues, and
 * work-stealing tails"). These cases exercise it the way its own documentation says it will be used.
 *
 * What is actually pinned:
 *   - **No loss, no duplication** under genuine multi-producer contention: the union of what the
 *     consumer drains must equal exactly what the producers pushed.
 *   - **Per-producer FIFO** — the Vyukov algorithm orders each producer's own items even though it
 *     makes no promise about interleaving between producers.
 *   - **`size()` never underflows.** `count_` is a `std::size_t`, so if the consumer's `fetch_sub`
 *     can be sequenced before the matching producer's `fetch_add`, the counter wraps to `SIZE_MAX`
 *     and `size()` reports ~1.8e19. The consumer samples `size()` on every iteration and the
 *     maximum is compared against the true item count.
 *
 *     Be honest about this one: the sample is a *cheap always-on invariant check*, NOT a reliable
 *     detector. The offending window (`push` publishing the node before incrementing the counter)
 *     is one instruction wide, so no portable test can be relied on to land in it — a standalone
 *     probe reproduced SIZE_MAX only intermittently. The guarantee that it cannot happen comes
 *     from the ordering argument documented at the `fetch_add` in `push()`, not from this
 *     assertion. The assertion is kept because it costs nothing and can never false-fail.
 *   - The **stalled-producer window**: `pop()` must spin for a producer preempted between its
 *     `tail_.exchange()` and its `prev->next.store()` rather than declaring the queue empty. Driven
 *     by keeping the queue near-empty with a hot consumer.
 *   - **Node reclamation**: `pop()` deletes the previous dummy node, and the destructor frees the
 *     rest. Payloads count their own live instances, so a missed or doubled destructor is visible
 *     as a non-zero balance (and ASan sees the node leak directly).
 *   - **Move-only and non-copyable payloads** instantiate at all (the compile-probe half).
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/lockfree/mpsc_unbounded_queue.h>

using qb::lockfree::mpsc_unbounded_queue;

namespace {

// ---------------------------------------------------------------------------
// Single-threaded contract
// ---------------------------------------------------------------------------

TEST(MpscUnboundedQueue, EmptyQueuePopsNothing) {
    mpsc_unbounded_queue<int> q;
    int                       out = -1;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.pop(out));
    EXPECT_EQ(out, -1) << "a failed pop must not touch the out parameter";
}

TEST(MpscUnboundedQueue, SingleThreadPreservesFifoOrder) {
    mpsc_unbounded_queue<int> q;
    for (int i = 0; i < 128; ++i)
        q.push(i);
    EXPECT_EQ(q.size(), 128u);
    EXPECT_FALSE(q.empty());

    for (int i = 0; i < 128; ++i) {
        int out = -1;
        ASSERT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
    int out = -1;
    EXPECT_FALSE(q.pop(out));
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

// ---------------------------------------------------------------------------
// Payload instantiation probes — the header only promises "movable type".
// ---------------------------------------------------------------------------

TEST(MpscUnboundedQueue, CarriesMoveOnlyPayload) {
    mpsc_unbounded_queue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(7));
    q.push(std::make_unique<int>(9));

    std::unique_ptr<int> out;
    ASSERT_TRUE(q.pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
    ASSERT_TRUE(q.pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 9);
    EXPECT_FALSE(q.pop(out));
}

// Live-instance counter so a missed / doubled payload destructor is observable.
std::atomic<int> g_payload_live{0};

struct CountedPayload {
    int value = 0;

    CountedPayload() {
        g_payload_live.fetch_add(1, std::memory_order_relaxed);
    }
    explicit CountedPayload(int v)
        : value(v) {
        g_payload_live.fetch_add(1, std::memory_order_relaxed);
    }
    CountedPayload(CountedPayload &&o) noexcept
        : value(o.value) {
        o.value = -1;
        g_payload_live.fetch_add(1, std::memory_order_relaxed);
    }
    CountedPayload &
    operator=(CountedPayload &&o) noexcept {
        value   = o.value;
        o.value = -1;
        return *this;
    }
    CountedPayload(CountedPayload const &)            = delete;
    CountedPayload &operator=(CountedPayload const &) = delete;
    ~CountedPayload() {
        g_payload_live.fetch_sub(1, std::memory_order_relaxed);
    }
};

TEST(MpscUnboundedQueue, DestructorReclaimsUndrainedPayloads) {
    g_payload_live.store(0, std::memory_order_relaxed);
    {
        mpsc_unbounded_queue<CountedPayload> q;
        for (int i = 0; i < 64; ++i)
            q.push(CountedPayload{i});

        CountedPayload out;
        for (int i = 0; i < 16; ++i) // drain only a quarter, abandon the rest to the destructor
            ASSERT_TRUE(q.pop(out));
        EXPECT_EQ(out.value, 15);
    }
    EXPECT_EQ(g_payload_live.load(std::memory_order_relaxed), 0)
        << "queue teardown must destroy the payloads still parked in its nodes";
}

// ---------------------------------------------------------------------------
// Genuine multi-producer contention.
//
// Encoding: item = producer_index * kPerProducer + sequence. That makes both checks cheap — the
// multiset of all values must be exactly [0, kProducers*kPerProducer), and each producer's own
// subsequence must come out ascending.
// ---------------------------------------------------------------------------

constexpr int kProducers   = 4;
constexpr int kPerProducer = 20000;
constexpr int kTotal       = kProducers * kPerProducer;

// Ceiling on the two consumer drain loops below. Both are hot busy-waits on "have I drained every
// item yet", so the ONE defect they exist to catch — a lost item — is also the one thing that makes
// their exit condition unreachable. Unbounded, that does not fail the test: it pegs a core until
// ctest's per-tier TIMEOUT (60s for tier:unit, `qb/cmake/qbFunctions.cmake` default) kills it, and
// the run is reported as `***Timeout`, indistinguishable in a CI log from an overloaded runner —
// while `"item N was lost or duplicated"`, the assertion written to name the defect, never runs.
// Falling through on the deadline hands the verdict to those assertions instead.
// 20s is ~1400x the measured 14ms whole-binary runtime, so it cannot fire on a slow or
// sanitizer-instrumented host, and it stays comfortably under the 60s tier timeout.
constexpr auto kDrainDeadline = std::chrono::seconds(20);

TEST(MpscUnboundedQueue, MultiProducerLosesNothingAndKeepsPerProducerOrder) {
    mpsc_unbounded_queue<int> q;
    std::atomic<bool>         go{false};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, &go, p] {
            while (!go.load(std::memory_order_acquire))
                qb::spin_loop_pause();
            for (int i = 0; i < kPerProducer; ++i)
                q.push(p * kPerProducer + i);
        });
    }

    // The consumer runs hot with no backoff, so the queue hovers near-empty: that is what drives
    // pop() into both the "looks empty but tail moved" branch and the stalled-producer spin.
    std::vector<int> drained;
    drained.reserve(kTotal);
    std::size_t max_size_seen = 0;

    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire))
            qb::spin_loop_pause();
        int        out      = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
        while (static_cast<int>(drained.size()) < kTotal && std::chrono::steady_clock::now() < deadline) {
            max_size_seen = std::max(max_size_seen, q.size());
            if (q.pop(out))
                drained.push_back(out);
        }
    });

    go.store(true, std::memory_order_release);
    for (auto &t : producers)
        t.join();
    consumer.join();

    // 1. Nothing lost, nothing duplicated.
    ASSERT_EQ(drained.size(), static_cast<std::size_t>(kTotal));
    std::vector<int> sorted = drained;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < kTotal; ++i)
        ASSERT_EQ(sorted[static_cast<std::size_t>(i)], i) << "item " << i << " was lost or duplicated";

    // 2. Per-producer FIFO.
    std::vector<int> last_seen(kProducers, -1);
    for (int v : drained) {
        const int p = v / kPerProducer;
        const int s = v % kPerProducer;
        ASSERT_GT(s, last_seen[static_cast<std::size_t>(p)]) << "producer " << p << " observed out of order";
        last_seen[static_cast<std::size_t>(p)] = s;
    }

    // 3. size() must never have wrapped. An unsigned underflow reports ~1.8e19, so any sample above
    //    the true item count means the consumer's fetch_sub raced ahead of the producer's fetch_add.
    EXPECT_LE(max_size_seen, static_cast<std::size_t>(kTotal))
        << "size() underflowed to " << max_size_seen << " (SIZE_MAX is " << std::numeric_limits<std::size_t>::max()
        << "): the producer publishes the node before it increments the counter";

    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(MpscUnboundedQueue, MultiProducerPayloadsAreDestroyedExactlyOnce) {
    g_payload_live.store(0, std::memory_order_relaxed);
    constexpr int kEach = 5000;
    {
        mpsc_unbounded_queue<CountedPayload> q;
        std::atomic<bool>                    go{false};
        std::atomic<int>                     drained{0};

        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&q, &go, p] {
                while (!go.load(std::memory_order_acquire))
                    qb::spin_loop_pause();
                for (int i = 0; i < kEach; ++i)
                    q.push(CountedPayload{p * kEach + i});
            });
        }
        std::thread consumer([&] {
            while (!go.load(std::memory_order_acquire))
                qb::spin_loop_pause();
            CountedPayload out;
            const auto     deadline = std::chrono::steady_clock::now() + kDrainDeadline;
            while (drained.load(std::memory_order_relaxed) < kProducers * kEach && std::chrono::steady_clock::now() < deadline) {
                if (q.pop(out))
                    drained.fetch_add(1, std::memory_order_relaxed);
            }
        });

        go.store(true, std::memory_order_release);
        for (auto &t : producers)
            t.join();
        consumer.join();
        EXPECT_EQ(drained.load(std::memory_order_relaxed), kProducers * kEach);
    }
    EXPECT_EQ(g_payload_live.load(std::memory_order_relaxed), 0)
        << "a payload was destroyed twice (negative) or never (positive) across the MPSC handoff";
}

} // namespace
