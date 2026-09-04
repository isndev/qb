/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/mailbox-park-handshake.cpp
 * @brief `SharedCoreCommunication::Mailbox::wait()` / `notify()` — the park/unpark handshake.
 *
 * The 3.0 form was `cv.wait_for(lk, latency)` with no predicate and `notify_all()` without the
 * mutex. An enqueue landing between the consumer's empty drain and its registration on the
 * condition variable was never seen, and the core slept the whole `latency`: measured on a
 * two-core ping-pong, 246–293 lost wakeups per 300 000 messages on Linux (1 ms each) and
 * 48–2849 on Windows, where MSVC's `wait_for` rounds up to the 15.6 ms scheduler tick (~13 ms
 * each, 50–95 % of all wait time). The handshake is now a Dekker pair over a `_parked` flag with
 * `seq_cst` fences on both sides, the producer taking the mutex before it notifies, and the
 * wait re-checking `has_data()` as its predicate. What this file pins:
 *
 *   - `wait()` returns at once when data is already published, and after `latency` when nothing
 *     arrives (bounded on both sides: the timeout is a real timeout, not a spin);
 *   - both halves are no-ops at latency 0 — a spin-mode mailbox never touches the mutex;
 *   - `mpsc::has_data()` sees an item on ANY producer ring and nothing on an empty set;
 *   - two threads bouncing one item between two mailboxes, parking on EVERY hop (the most
 *     adversarial cadence for the race) with a latency far above the cost of a hop, never
 *     observe a wait that lasts the latency: a single lost wakeup would.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/main.h>

namespace mailbox_park_handshake_test {

using namespace std::chrono_literals;
using Mailbox = qb::SharedCoreCommunication::Mailbox;
using Clock   = std::chrono::steady_clock;

constexpr std::size_t kProducers = 2;

EventBucket
bucket(std::uint32_t const tag) {
    EventBucket b{};
    b.__raw__[0] = tag;
    return b;
}

// ---- the two halves, alone ------------------------------------------------------------------

TEST(MailboxParkHandshake, WaitReturnsAtOnceWhenDataIsAlreadyPublished) {
    Mailbox    mb(kProducers, 200ms, 0us);
    const auto b = bucket(1);
    ASSERT_EQ(mb.enqueue(1, &b, 1), 1u);
    EXPECT_TRUE(mb.has_data());
    const auto t0 = Clock::now();
    mb.wait();
    EXPECT_LT(Clock::now() - t0, 100ms) << "a wait with data published must not block";
}

TEST(MailboxParkHandshake, WaitTimesOutAtLatencyWhenNothingArrives) {
    Mailbox    mb(kProducers, 30ms, 0us);
    const auto t0 = Clock::now();
    mb.wait();
    const auto elapsed = Clock::now() - t0;
    EXPECT_GE(elapsed, 25ms) << "the wait is a real park, not a spin"; // MSVC rounds to ms
    EXPECT_LT(elapsed, 2s) << "and the timeout is honoured";
}

TEST(MailboxParkHandshake, ZeroLatencyIsSpinModeAndBothHalvesAreNoOps) {
    Mailbox mb(kProducers, 0ns, 0us);
    EXPECT_EQ(mb.getLatency(), 0ns);
    const auto t0 = Clock::now();
    for (int i = 0; i < 1000; ++i)
        mb.wait();
    mb.notify();
    EXPECT_LT(Clock::now() - t0, 100ms) << "a spin-mode mailbox never parks";
}

TEST(MailboxParkHandshake, ConfigIsCarried) {
    Mailbox mb(kProducers, 250us, 50us);
    EXPECT_EQ(mb.getLatency(), 250us);
    EXPECT_EQ(mb.getIdleSpin(), 50us);
}

TEST(MailboxParkHandshake, HasDataSeesAnyProducerRing) {
    Mailbox mb(kProducers, 1ms, 0us);
    EXPECT_FALSE(mb.has_data());
    const auto b = bucket(7);
    ASSERT_EQ(mb.enqueue(kProducers - 1, &b, 1), 1u); // the LAST ring, not the first
    EXPECT_TRUE(mb.has_data());
    EventBucket out[1];
    ASSERT_EQ(mb.dequeue(out, 1), 1u);
    EXPECT_FALSE(mb.has_data());
}

// ---- the race, hammered -----------------------------------------------------------------------

TEST(MailboxParkHandshake, PingPongParkingOnEveryHopNeverLosesAWakeup) {
    // A lost wakeup costs the whole latency; a hop costs microseconds. Any wait that lasts
    // `kLatency` is therefore a loss, and the max is asserted, not the mean.
    constexpr auto kLatency = 250ms;
    constexpr int  kHops    = 40000;

    Mailbox to_b(kProducers, kLatency, 0us); // A → B
    Mailbox to_a(kProducers, kLatency, 0us); // B → A

    std::atomic<std::int64_t> worst_wait_ns{0};
    std::atomic<int>          waits{0};
    auto                      hop = [&](Mailbox &in, Mailbox &out, std::size_t const producer, int const count) {
        EventBucket out_b[1];
        for (int i = 0; i < count; ++i) {
            const auto t0 = Clock::now();
            while (in.dequeue(out_b, 1) == 0) // park on EVERY empty poll: the adversarial cadence
                in.wait();
            const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
            ++waits;
            for (auto cur = worst_wait_ns.load(std::memory_order_relaxed);
                 waited > cur && !worst_wait_ns.compare_exchange_weak(cur, waited, std::memory_order_relaxed);)
                ;
            const auto b = bucket(static_cast<std::uint32_t>(i));
            ASSERT_EQ(out.enqueue(producer, &b, 1), 1u);
            out.notify();
        }
    };

    std::thread b([&] { hop(to_b, to_a, 1, kHops); });
    // A serves: it sends first, then answers each reply. Same count, so both threads end together.
    {
        const auto first = bucket(0);
        ASSERT_EQ(to_b.enqueue(0, &first, 1), 1u);
        to_b.notify();
        hop(to_a, to_b, 0, kHops - 1);
        EventBucket last[1];
        while (to_a.dequeue(last, 1) == 0)
            to_a.wait();
    }
    b.join();

    EXPECT_EQ(waits.load(), 2 * kHops - 1);
    const auto worst = std::chrono::nanoseconds{worst_wait_ns.load()};
    EXPECT_LT(worst, kLatency / 2) << "a wait that reached the latency is a lost wakeup; worst = " << worst.count() << " ns";
}

} // namespace mailbox_park_handshake_test
