/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/oversize-event-probe.cpp
 * @brief An event too large for the destination mailbox ring must not wedge the engine.
 *
 * A cross-core event is delivered by copying its whole bucket run into the destination
 * core's MPSC mailbox ring. That ring holds `65535 / QB_LOCKFREE_EVENT_BUCKET_BYTES`
 * usable buckets (1023 with the default 64-byte bucket), and `spsc::enqueue<_All=true>`
 * is all-or-nothing — so an event of 1024+ buckets can **never** be enqueued, no matter
 * how much room the consumer frees.
 *
 * `__flush_all__` treats a failed `try_send` as transient backpressure and retries with a
 * bounded budget, then partial-bails and retries again next loop pass. For a *permanently*
 * unsendable event that retry never converges:
 *   - the outbound pipe is FIFO, so every later event to that core is stuck behind it
 *     (head-of-line block) — including the one that would have killed the receiver;
 *   - the sender core leaves its main loop, enters the shutdown residual drain, and spins
 *     there forever because the destination core is still "live";
 *   - the destination core never terminates because its stop event never arrives.
 * Result: `qb::Main::join()` never returns and two cores burn 100% CPU, with no diagnostic.
 *
 * Pinned here at the exact boundary: `kMaxBuckets` buckets must deliver, `kMaxBuckets + 1`
 * must NOT wedge the engine. The oversize case is run on a detached thread with a watchdog
 * so a regression fails this test instead of hanging the whole suite.
 */

#include <atomic>
#include <chrono>
#include <string>
#include <cstdio>
#include <future>
#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

/// Usable buckets in a destination core's mailbox ring (mirrors SharedCoreCommunication).
constexpr std::size_t kMaxBuckets = 65535u / QB_LOCKFREE_EVENT_BUCKET_BYTES;

std::atomic<int> g_big{0};
std::atomic<int> g_tail{0};
std::atomic<int> g_payload_alive{0};

/// Non-trivially-destructible payload: proves the drop path runs the event's destructor exactly
/// once instead of leaking the heap it owns. Counted rather than leak-checked so the assertion
/// holds on every platform (macOS ASan does no leak detection).
struct TrackedPayload {
    std::string blob;
    TrackedPayload()
        : blob(256, 'x') {
        g_payload_alive.fetch_add(1, std::memory_order_relaxed);
    }
    TrackedPayload(TrackedPayload const &rhs)
        : blob(rhs.blob) {
        g_payload_alive.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedPayload() {
        g_payload_alive.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct BigEvent : public qb::Event {
    TrackedPayload payload;
};
/// Queued into the SAME pipe right after BigEvent: proves ordered traffic still flows.
struct TailEvent : public qb::Event {};

class OvRecv final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<BigEvent>(*this);
        registerEvent<TailEvent>(*this);
        co_return true;
    }
    void
    on(BigEvent const &) {
        g_big.fetch_add(1, std::memory_order_relaxed);
    }
    void
    on(TailEvent const &) {
        g_tail.fetch_add(1, std::memory_order_relaxed);
        kill();
    }
};

class OvSend final : public qb::Actor {
    const qb::ActorId _to;
    const std::size_t _extra;

public:
    OvSend(qb::ActorId to, std::size_t extra)
        : _to(to)
        , _extra(extra) {}
    qb::io::async::task<bool>
    onInit() override {
        auto pipe = getPipe(_to);
        static_cast<void>(pipe.allocated_push<BigEvent>(_extra));
        pipe.push<TailEvent>(); // FIFO behind BigEvent — must still get through
        kill();
        co_return true;
    }
};

/// `allocated_push` computes ceil((extra + sizeof(T)) / bucket) buckets — invert it.
constexpr std::size_t
extra_for_buckets(std::size_t buckets) noexcept {
    return buckets * QB_LOCKFREE_EVENT_BUCKET_BYTES - sizeof(BigEvent);
}

/// Run one cross-core case; returns false if the engine failed to terminate in `budget`.
[[nodiscard]] bool
run_case(std::size_t extra, std::chrono::seconds budget) {
    g_big          = 0;
    g_tail         = 0;
    g_payload_alive = 0;
    // The engine runs on its own thread: a wedged join() must fail this test, not hang the
    // suite. std::async's future would block in its destructor, so use a detached thread
    // plus a promise.
    auto  done   = std::make_shared<std::promise<void>>();
    auto  future = done->get_future();
    std::thread([extra, done] {
        qb::Main main;
        auto     rcv = main.addActor<OvRecv>(1);
        main.addActor<OvSend>(0, rcv, extra);
        main.start();
        main.join();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

} // namespace

// The largest event that still fits the ring must be delivered intact.
TEST(OversizeEvent, MaxSizedEventIsDeliveredCrossCore) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: needs a second core to exercise cross-core delivery";

    ASSERT_TRUE(run_case(extra_for_buckets(kMaxBuckets), std::chrono::seconds(30))) << "engine did not terminate for a ring-sized event";
    EXPECT_EQ(g_big.load(), 1) << "an event of exactly " << kMaxBuckets << " buckets must fit the mailbox ring";
    EXPECT_EQ(g_tail.load(), 1);
}

// One bucket more can never be enqueued — the engine must still terminate, and the
// ordered traffic queued behind it must not be held hostage.
TEST(OversizeEvent, OversizedEventDoesNotWedgeTheEngine) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: needs a second core to exercise cross-core delivery";

    ASSERT_TRUE(run_case(extra_for_buckets(kMaxBuckets + 1), std::chrono::seconds(30)))
        << "engine wedged: an event of " << (kMaxBuckets + 1) << " buckets can never be enqueued into the "
        << kMaxBuckets << "-bucket mailbox ring, so __flush_all__ retries it forever and join() never returns";
    EXPECT_EQ(g_big.load(), 0) << "an oversized event cannot be delivered cross-core";
    EXPECT_EQ(g_tail.load(), 1) << "traffic queued behind the undeliverable event must still be delivered "
                                   "(no head-of-line block)";
    // The dropped event owns heap through TrackedPayload. Dropping it must run its destructor
    // exactly once (via the router's type-erased disposer), not leak it and not double-free it.
    EXPECT_EQ(g_payload_alive.load(), 0) << "the undeliverable event's payload was not disposed — dropping it must free "
                                            "its heap exactly like every other terminal path";
}
