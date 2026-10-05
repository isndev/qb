/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/core-stats.cpp
 * @brief `Actor::getCoreStats()`: what a core counts, and that every count conserves (Huly QB-162).
 *
 * The counters existed before 3.3 as per-pass values the idle policy read and then cleared, so a
 * cumulative figure did not exist to read; two of them also said something false. A cross-core
 * `send` / `reply` / `forward` goes straight into the peer's ring and no flush ever sees it, so it
 * was never counted as sent -- every reply of a two-core ping-pong was invisible -- and an
 * `EventQOS0` the flush dropped on backpressure was counted as SENT. What this file pins, each
 * count exact rather than "some":
 *
 *   - a two-core exchange: every `push` (published by the flush) and every `reply` (published
 *     directly) is counted sent once, every arrival received once, with its width in buckets;
 *   - the same exchange on ONE core sends nothing: same-core traffic is received, never sent;
 *   - an `EventQOS0` burst into a full mailbox conserves: published + dropped == produced, the
 *     peer receives exactly what was published, and each drop met a full mailbox once
 *     (`sends_blocked`) -- no system test covered the QoS-0 drop at all before this one;
 *   - a QoS-2 burst into a full mailbox loses nothing and reports the backpressure it met;
 *   - io callbacks are counted (a `defer()`ed one included), and `loop_passes` is the pass
 *     number a `LoopEvent` carries.
 *
 * A counter moves AFTER the event it counts was handled, so a snapshot taken inside the handler
 * of the k-th arrival reads k - 1 received. The snapshots are written to globals and read after
 * `join()`, which orders them.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/main.h>

namespace core_stats_test {

using namespace std::chrono_literals;

struct Ball : qb::Event {};
constexpr std::uint64_t kBallWidth = qb::allocator::getItemSize<Ball, EventBucket>();

qb::CoreStats g_initiator{};
qb::CoreStats g_responder{};

// Answers each ball in place: `reply()` publishes it straight into the initiator's ring when the
// initiator is on another core, and into this core's own queue when it is not.
class Responder : public qb::Actor {
    std::uint64_t _rounds;
    std::uint64_t _seen = 0;

public:
    explicit Responder(std::uint64_t rounds) noexcept
        : _rounds(rounds) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ball>(*this);
        co_return true;
    }
    void
    on(Ball &ball) {
        reply(ball);
        if (++_seen == _rounds) {
            g_responder = getCoreStats();
            kill();
        }
    }
};

// Serves each ball with a fresh `push` -- published by the flush on the next pass.
class Initiator : public qb::Actor {
    qb::ActorId   _peer;
    std::uint64_t _rounds;
    std::uint64_t _returned = 0;

public:
    Initiator(qb::ActorId peer, std::uint64_t rounds) noexcept
        : _peer(peer)
        , _rounds(rounds) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ball>(*this);
        push<Ball>(_peer);
        co_return true;
    }
    void
    on(Ball const &) {
        if (++_returned == _rounds) {
            g_initiator = getCoreStats();
            kill();
            return;
        }
        push<Ball>(_peer);
    }
};

TEST(CoreStats, ACrossCoreExchangeCountsEveryPushAndEveryReply) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the exchange must cross a mailbox";
    constexpr std::uint64_t kRounds = 1000;
    g_initiator                     = {};
    g_responder                     = {};

    qb::Main   main;
    const auto responder = main.addActor<Responder>(1, kRounds);
    main.addActor<Initiator>(0, responder, kRounds);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    // The initiator, inside the handler of the last ball back: kRounds pushes published, the
    // returning balls received but for the one being handled.
    EXPECT_EQ(g_initiator.events_sent, kRounds);
    EXPECT_EQ(g_initiator.buckets_sent, kRounds * kBallWidth);
    EXPECT_EQ(g_initiator.events_received, kRounds - 1);
    EXPECT_EQ(g_initiator.buckets_received, (kRounds - 1) * kBallWidth);
    // The responder, after its last reply: every reply went straight into the initiator's ring
    // (one ball in flight cannot fill it), and none of them was counted before 3.3.
    EXPECT_EQ(g_responder.events_sent, kRounds) << "a direct cross-core reply must be counted as sent";
    EXPECT_EQ(g_responder.buckets_sent, kRounds * kBallWidth);
    EXPECT_EQ(g_responder.events_received, kRounds - 1);
    for (auto const *s : {&g_initiator, &g_responder}) {
        EXPECT_EQ(s->sends_blocked, 0u);
        EXPECT_EQ(s->events_dropped, 0u);
        EXPECT_GT(s->loop_passes, 0u);
    }
}

TEST(CoreStats, SameCoreTrafficIsReceivedAndNeverSent) {
    constexpr std::uint64_t kRounds = 1000;
    g_initiator                     = {};

    qb::Main   main;
    const auto responder = main.addActor<Responder>(0, kRounds);
    main.addActor<Initiator>(0, responder, kRounds);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    // Both actors share core 0: every ball the core handled is counted received -- kRounds to
    // the responder, kRounds - 1 back to the initiator so far -- and none of them left the core.
    EXPECT_EQ(g_initiator.events_sent, 0u);
    EXPECT_EQ(g_initiator.buckets_sent, 0u);
    EXPECT_EQ(g_initiator.events_received, 2 * kRounds - 1);
    EXPECT_EQ(g_initiator.buckets_received, (2 * kRounds - 1) * kBallWidth);
}

// --- a burst into a mailbox its consumer is not draining ----------------------------------------
// The sink blocks its own core inside the handler of `Hold`; the source, seeing it hold, pushes a
// burst four times the ring's capacity, then a self-addressed `Check`. The flush of the next pass
// meets the burst first and the `Check` is received after it, in the same pass: at `Check` the
// whole burst has been published or dropped. `Tally` then tells the sink how many were published.

// A producer's ring in a peer's mailbox holds `max_deliverable_buckets` (1023 with 64-byte buckets).
constexpr std::uint64_t kRing  = qb::detail::max_deliverable_buckets;
constexpr std::uint64_t kBurst = 4 * kRing;

struct Hold : qb::Event {};
struct Spark : qb::EventQOS0 {};
struct Load : qb::Event {};
struct Check : qb::Event {};
struct Tally : qb::Event {
    std::uint64_t published;
    explicit Tally(std::uint64_t n) noexcept
        : published(n) {}
};

std::atomic<bool> g_sink_holding{false};
std::uint64_t     g_sink_received = 0;
std::uint64_t     g_tally         = 0;
qb::CoreStats     g_burst_before{};
qb::CoreStats     g_burst_after{};

template <typename Burst>
class Sink : public qb::Actor {
    std::chrono::milliseconds _hold;
    std::uint64_t             _received = 0;

public:
    explicit Sink(std::chrono::milliseconds hold) noexcept
        : _hold(hold) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Hold>(*this);
        registerEvent<Burst>(*this);
        registerEvent<Tally>(*this);
        co_return true;
    }
    void
    on(Hold const &) {
        g_sink_holding.store(true, std::memory_order_release);
        std::this_thread::sleep_for(_hold); // blocks this core: its mailbox fills
    }
    void
    on(Burst const &) {
        ++_received;
    }
    void
    on(Tally const &tally) {
        g_sink_received = _received;
        g_tally         = tally.published;
        kill();
    }
};

template <typename Burst>
class Source
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _sink;
    bool        _fired = false;

public:
    explicit Source(qb::ActorId sink) noexcept
        : _sink(sink) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Check>(*this);
        push<Hold>(_sink);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (_fired || !g_sink_holding.load(std::memory_order_acquire))
            return;
        _fired = true;
        unregisterCallback();
        g_burst_before = getCoreStats();
        for (std::uint64_t i = 0; i < kBurst; ++i)
            push<Burst>(_sink);
        push<Check>(id());
    }
    void
    on(Check const &) {
        g_burst_after = getCoreStats();
        push<Tally>(_sink, g_burst_after.events_sent - g_burst_before.events_sent);
        kill();
    }
};

TEST(CoreStats, AQoS0BurstIntoAFullMailboxIsPublishedOrDroppedNeverBoth) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the burst must cross a mailbox";
    g_sink_holding.store(false);
    g_sink_received = g_tally = 0;
    g_burst_before = g_burst_after = {};

    qb::Main   main;
    const auto sink = main.addActor<Sink<Spark>>(1, 250ms);
    main.addActor<Source<Spark>>(0, sink);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    const auto published = g_burst_after.events_sent - g_burst_before.events_sent;
    const auto dropped   = g_burst_after.events_dropped - g_burst_before.events_dropped;
    const auto blocked   = g_burst_after.sends_blocked - g_burst_before.sends_blocked;
    EXPECT_EQ(published + dropped, kBurst) << "every QoS-0 event is published or dropped, and counted once";
    EXPECT_GT(dropped, 0u) << "a burst four rings deep into a blocked core must overflow";
    EXPECT_LE(published, kRing) << "no more than one ring's worth can have been published into a blocked core";
    EXPECT_EQ(blocked, dropped) << "each drop met a full mailbox exactly once";
    EXPECT_EQ(g_tally, published);
    EXPECT_EQ(g_sink_received, published) << "the sink receives exactly what was counted published";
}

TEST(CoreStats, AQoS2BurstIntoAFullMailboxLosesNothingAndReportsTheBackpressure) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the burst must cross a mailbox";
    g_sink_holding.store(false);
    g_sink_received = g_tally = 0;
    g_burst_before = g_burst_after = {};

    qb::Main   main;
    const auto sink = main.addActor<Sink<Load>>(1, 100ms);
    main.addActor<Source<Load>>(0, sink);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    // A QoS-2 event is retried until it goes: at `Check` some of the burst may still be queued,
    // so the conservation is read at the sink, which drains everything before `Tally`.
    EXPECT_EQ(g_sink_received, kBurst) << "nothing is dropped on backpressure but an EventQOS0";
    EXPECT_EQ(g_burst_after.events_dropped - g_burst_before.events_dropped, 0u);
    EXPECT_GT(g_burst_after.sends_blocked - g_burst_before.sends_blocked, 0u)
        << "a burst four rings deep into a blocked core must meet a full mailbox";
}

// --- io callbacks and passes --------------------------------------------------------------------

std::atomic<bool> g_deferred_ran{false};
qb::CoreStats     g_io_before{};
qb::CoreStats     g_io_after{};
std::uint64_t     g_io_iteration = 0;

class IoProbe
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_io_before = getCoreStats();
        // Captures nothing the actor owns: it only flips a global.
        qb::io::async::defer([] { g_deferred_ran.store(true, std::memory_order_relaxed); });
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &ev) override {
        if (!g_deferred_ran.load(std::memory_order_relaxed))
            return;
        g_io_after     = getCoreStats();
        g_io_iteration = ev.iteration;
        kill();
    }
};

TEST(CoreStats, IoCallbacksAndPassesAreCounted) {
    g_deferred_ran.store(false);
    g_io_before = g_io_after = {};
    g_io_iteration           = 0;

    qb::Main main;
    main.addActor<IoProbe>(0);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    EXPECT_GE(g_io_after.io_events, g_io_before.io_events + 1) << "the deferred callback is an io event of this core";
    EXPECT_EQ(g_io_after.loop_passes, g_io_iteration) << "loop_passes is the pass number a LoopEvent carries";
    EXPECT_GT(g_io_after.loop_passes, g_io_before.loop_passes);
}

} // namespace core_stats_test
