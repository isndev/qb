/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/push-reference-stability.cpp
 * @brief The reference `Actor::push` / `Pipe::push` returns stays valid until the handler returns.
 *
 * Until the event queues became segmented (`qb::VirtualPipe` = `allocator::segmented_pipe`), a
 * reference to a queued event died at the very next push reaching the same destination core: the
 * outbound pipe doubled and memcpy'd, and the earlier reference dangled. The documented contract
 * now is the one a user would expect — populate the event at any point before the handler
 * returns, whatever else was pushed in between — and this file is what pins it at engine level.
 * Every test keeps a reference to EVERY event it queues, pushes enough to force several segment
 * links (a standard segment is 256 KB, 4095 buckets), then writes through all of them AFTER the
 * queue has grown; the receiver asserts every event arrived carrying the late write, in order.
 *
 * Three delivery paths are exercised because they walk the pipe differently:
 *   - same core: `__receive__` swaps the mono pipe and routes segment by segment;
 *   - cross core: `__flush_all__` builds runs from `front()` and publishes them into the ring;
 *   - a jumbo `allocated_push` wider than a segment: a dedicated exactly-sized segment.
 * The fourth test re-pushes from INSIDE a handler while `__receive__` is walking the same
 * core's pipe, which is where the pool's cache-warm reuse happens and where a segment released
 * too early would hand the writer memory the reader is still routing.
 *
 * The allocator-level half of the same contract is `SegmentedPipeContract.*`
 * (tests/io/unit/core/segmented-pipe.cpp).
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

constexpr std::uint64_t kMagic = 0x5EC0DE5EC0DE5EC0ull;
/// A standard segment holds 4095 buckets; three links plus a remainder.
constexpr std::size_t kBurst = 3 * 4096 + 7;

std::atomic<std::size_t> g_seen{0};
std::atomic<std::size_t> g_bad{0};
std::atomic<std::size_t> g_jumbo_ok{0};

struct SeqEvent : public qb::Event {
    std::uint64_t seq   = 0;
    std::uint64_t stamp = 0;
};

/// Trailing bytes follow the event; `extra` says how many and `sum` what they must add up to.
struct JumboEvent : public qb::Event {
    std::size_t   extra = 0;
    std::uint64_t sum   = 0;
    std::uint64_t stamp = 0;
};

class Receiver final : public qb::Actor {
    const std::size_t _expected;
    std::uint64_t     _next = 0;

public:
    explicit Receiver(std::size_t expected)
        : _expected(expected) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SeqEvent>(*this);
        registerEvent<JumboEvent>(*this);
        co_return true;
    }

    void
    on(SeqEvent const &e) {
        // Both fields were written AFTER every later push: a moved event would carry zeros.
        if (e.stamp != kMagic || e.seq != _next)
            g_bad.fetch_add(1, std::memory_order_relaxed);
        ++_next;
        if (g_seen.fetch_add(1, std::memory_order_relaxed) + 1 == _expected)
            kill();
    }

    void
    on(JumboEvent const &e) {
        auto const   *bytes = reinterpret_cast<unsigned char const *>(&e) + sizeof(JumboEvent);
        std::uint64_t sum   = 0;
        for (std::size_t i = 0; i < e.extra; ++i)
            sum += bytes[i];
        if (e.stamp == kMagic && sum == e.sum && e.extra != 0)
            g_jumbo_ok.fetch_add(1, std::memory_order_relaxed);
        else
            g_bad.fetch_add(1, std::memory_order_relaxed);
    }
};

/// Queues `kBurst` events keeping every reference, then writes through all of them.
class BurstSender final : public qb::Actor {
    const qb::ActorId _to;

public:
    explicit BurstSender(qb::ActorId to)
        : _to(to) {}

    qb::io::async::task<bool>
    onInit() override {
        std::vector<SeqEvent *> refs;
        refs.reserve(kBurst);
        auto pipe = getPipe(_to);
        for (std::size_t i = 0; i < kBurst; ++i)
            refs.push_back(&pipe.push<SeqEvent>());
        // Every reference is written after the queue grew past several segments.
        for (std::size_t i = 0; i < kBurst; ++i) {
            refs[i]->seq   = i;
            refs[i]->stamp = kMagic;
        }
        kill();
        co_return true;
    }
};

/// One jumbo `allocated_push` (wider than a segment), then a burst behind it, then the jumbo's
/// trailing bytes and header are written last.
class JumboSender final : public qb::Actor {
    const qb::ActorId _to;
    const std::size_t _extra;

public:
    JumboSender(qb::ActorId to, std::size_t extra)
        : _to(to)
        , _extra(extra) {}

    qb::io::async::task<bool>
    onInit() override {
        auto                    pipe  = getPipe(_to);
        auto                   &jumbo = pipe.allocated_push<JumboEvent>(_extra);
        std::vector<SeqEvent *> refs;
        refs.reserve(kBurst);
        for (std::size_t i = 0; i < kBurst; ++i)
            refs.push_back(&pipe.push<SeqEvent>());

        auto *const   bytes = reinterpret_cast<unsigned char *>(&jumbo) + sizeof(JumboEvent);
        std::uint64_t sum   = 0;
        for (std::size_t i = 0; i < _extra; ++i) {
            bytes[i] = static_cast<unsigned char>(i * 7u + 3u);
            sum += bytes[i];
        }
        jumbo.extra = _extra;
        jumbo.sum   = sum;
        jumbo.stamp = kMagic;
        for (std::size_t i = 0; i < kBurst; ++i) {
            refs[i]->seq   = i;
            refs[i]->stamp = kMagic;
        }
        kill();
        co_return true;
    }
};

/// Re-pushes to ITSELF from inside its handler: each received event fans out into more, every
/// reference written after the fan-out, until `_total` events have been seen. The same core's
/// `__receive__` is walking the pipe these pushes grow, and the segments it releases are the
/// ones the pushes land in.
class Repusher final : public qb::Actor {
    const std::size_t _total;
    std::uint64_t     _next_seq = 0;
    std::uint64_t     _expect   = 0;
    std::size_t       _seen     = 0;

    void
    fan_out(std::size_t const n) {
        std::vector<SeqEvent *> refs;
        refs.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            refs.push_back(&push<SeqEvent>(id()));
        for (auto *const e : refs) {
            e->seq   = _next_seq++;
            e->stamp = kMagic;
        }
    }

public:
    explicit Repusher(std::size_t total)
        : _total(total) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SeqEvent>(*this);
        fan_out(1);
        co_return true;
    }

    void
    on(SeqEvent const &e) {
        if (e.stamp != kMagic || e.seq != _expect)
            g_bad.fetch_add(1, std::memory_order_relaxed);
        ++_expect;
        ++_seen;
        g_seen.fetch_add(1, std::memory_order_relaxed);
        if (_seen >= _total) {
            kill();
            return;
        }
        // Grow faster than the pass consumes: 2 in, 1 out, capped at the total.
        std::size_t const remaining = _total - _next_seq;
        if (remaining != 0)
            fan_out(remaining < 2 ? remaining : 2);
    }
};

template <typename Build>
[[nodiscard]] bool
run(Build build, std::chrono::seconds budget) {
    g_seen = g_bad = g_jumbo_ok = 0;
    auto done                   = std::make_shared<std::promise<void>>();
    auto future                 = done->get_future();
    std::thread([build, done] {
        qb::Main main;
        build(main);
        main.start();
        main.join();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

} // namespace

/**
 * @test Same core: `__receive__` routes the swapped mono pipe segment by segment; every reference
 *       written after 3+ segment links arrives intact and in order.
 */
TEST(PushReferenceStability, SameCoreBurstKeepsEveryReference) {
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            auto rcv = main.addActor<Receiver>(0, kBurst);
            main.addActor<BurstSender>(0, rcv);
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(g_seen.load(), kBurst);
    EXPECT_EQ(g_bad.load(), 0u) << "an event arrived without the write made through its reference "
                                   "after later pushes: the queue moved it";
}

/**
 * @test Cross core: `__flush_all__` publishes runs built from `front()`; same assertion.
 */
TEST(PushReferenceStability, CrossCoreBurstKeepsEveryReference) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore";
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            auto rcv = main.addActor<Receiver>(1, kBurst);
            main.addActor<BurstSender>(0, rcv);
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(g_seen.load(), kBurst);
    EXPECT_EQ(g_bad.load(), 0u);
}

/**
 * @test A jumbo `allocated_push` wider than a standard segment sits in a dedicated segment; its
 *       trailing bytes are written after a burst of later pushes and arrive intact.
 * @brief Same core only for the dedicated-segment width: a cross-core event is capped at
 *        `kMaxDeliverableBuckets` (1023, the ring's width — `oversize-event-probe.cpp` pins the
 *        rejection), so the widest event that can ever cross cores fits a standard segment. The
 *        cross-core half therefore uses exactly that ceiling, which is the widest run
 *        `__flush_all__` ever publishes from a segment.
 */
TEST(PushReferenceStability, JumboAllocatedPushIsStable) {
    // 5000 buckets: wider than the 4095-bucket standard segment, well under the 65535 ceiling.
    static constexpr std::size_t dedicated = 5000 * QB_LOCKFREE_EVENT_BUCKET_BYTES - sizeof(JumboEvent);
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            auto rcv = main.addActor<Receiver>(0, kBurst);
            main.addActor<JumboSender>(0, rcv, dedicated);
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(g_jumbo_ok.load(), 1u) << "the dedicated segment's trailing bytes did not survive the burst behind it";
    EXPECT_EQ(g_seen.load(), kBurst);
    EXPECT_EQ(g_bad.load(), 0u);

    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore for the cross-core half";
    // 1023 buckets: `SharedCoreCommunication::MaxRingEvents`, the ring's width.
    static constexpr std::size_t ring_max = 1023 * QB_LOCKFREE_EVENT_BUCKET_BYTES - sizeof(JumboEvent);
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            auto rcv = main.addActor<Receiver>(1, kBurst);
            main.addActor<JumboSender>(0, rcv, ring_max);
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(g_jumbo_ok.load(), 1u);
    EXPECT_EQ(g_seen.load(), kBurst);
    EXPECT_EQ(g_bad.load(), 0u);
}

/**
 * @test Re-pushing to self from inside the handler, while the same core's receive pass is walking
 *       the pipe: references written after the fan-out arrive intact and in FIFO order across
 *       many passes, and the segments recycled under the walk never alias what is still being
 *       routed.
 */
TEST(PushReferenceStability, SameCoreRepushUnderReceiveWalk) {
    static constexpr std::size_t total = 20 * 4096;
    ASSERT_TRUE(run([](qb::Main &main) { main.addActor<Repusher>(0, total); }, std::chrono::seconds(60)));
    EXPECT_EQ(g_seen.load(), total);
    EXPECT_EQ(g_bad.load(), 0u);
}
