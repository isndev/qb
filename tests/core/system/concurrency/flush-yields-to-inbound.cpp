/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/concurrency/flush-yields-to-inbound.cpp
 * @brief A core blocked on a peer's full mailbox drains its own inbound work instead of burning
 *        the pass in the flush's backoff (Huly QB-979).
 *
 * The shape savina/barber found: one core holds its peer's ring full for the length of a long
 * handler, and the peer -- which has work of its own queued -- used to spend every pass retrying
 * the head of its outbound pipe up to 512 times (64 pauses, then 448 yields) before it got to that
 * work. Here core 1's actor floods core 0's actor with pushes four rings deep while core 0 sleeps
 * in its first handler, and runs a self-addressed chain beside the flood: the chain is inbound
 * work the flush must yield to.
 *
 * What is pinned is a count, never a time. With the rule, a blocked pass tries the head event ONCE
 * and gives the pass back, so `sends_blocked` can never exceed `loop_passes`; without it a blocked
 * pass counted 513 attempts and the inequality fails by two orders of magnitude on the first
 * blocked pass (the red witness). The flood must have met a full ring at all (`sends_blocked > 0`,
 * which is why it is four rings deep: the batch that holds the sleeping handler's event was copied
 * out of the ring before the handler ran, so one more ring's worth goes in before the block), and
 * it must arrive whole and in order: the rule is the partial bail the exhausted budget already
 * took, taken earlier.
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/main.h>

namespace flush_yields_to_inbound_test {

using namespace std::chrono_literals;

struct Flood : qb::Event {
    std::uint64_t seq;
    explicit Flood(std::uint64_t s) noexcept
        : seq(s) {}
};
struct Step : qb::Event {};
struct Done : qb::Event {
    std::uint64_t received;
    explicit Done(std::uint64_t r) noexcept
        : received(r) {}
};

constexpr std::uint64_t kFlood = 4096; // four 1023-bucket mailbox rings deep, one bucket each
// The flush's budget on a refusal it cannot call a stall: one try_send, then 512 retries (VirtualCore.cpp's
// kFlushYieldAttempts), spent on every refusal that finds the peer's index MOVED since the last one and on the
// first of an episode. The sink moves its index once per batch it drains, and it drains kFlood / 1023 batches
// after the sleep (fast handlers, but under a sanitizer slow enough for the flooder to be refused between two of
// them), so the episode can buy that many budgets plus the two the stall rule always spends.
constexpr std::uint64_t kBudgets     = kFlood / 1023 + 2;
constexpr std::uint64_t kBudgetBound = kBudgets * 513;

qb::CoreStats g_flooder{};
std::uint64_t g_received = 0;
std::uint64_t g_in_order = 0;
std::uint64_t g_steps    = 0;

// Core 0: the first Flood is a long handler, as the barber's factory is one handler for the whole
// production; the ring fills behind it and stays full until the handler returns.
class Sink : public qb::Actor {
    std::uint64_t _received = 0;
    std::uint64_t _in_order = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Flood>(*this);
        co_return true;
    }
    void
    on(Flood const &flood) {
        if (flood.seq == _received)
            ++_in_order;
        if (_received++ == 0)
            std::this_thread::sleep_for(200ms);
        if (_received == kFlood) {
            g_received = _received;
            g_in_order = _in_order;
            push<Done>(flood.getSource(), _received);
            kill();
        }
    }
};

// Core 1: the flood, pushed before the loop's first pass, and a self-addressed chain that makes
// every pass of this core one with inbound work pending at flush time.
class Flooder : public qb::Actor {
    qb::ActorId _sink;
    bool        _done = false;

public:
    explicit Flooder(qb::ActorId sink) noexcept
        : _sink(sink) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Step>(*this);
        registerEvent<Done>(*this);
        for (std::uint64_t i = 0; i < kFlood; ++i)
            push<Flood>(_sink, i);
        push<Step>(id());
        co_return true;
    }
    void
    on(Step const &) {
        ++g_steps;
        if (_done) {
            kill();
            return;
        }
        push<Step>(id());
    }
    void
    on(Done const &) {
        // Read in the handler of the sink's answer: every Flood was published by then, so the
        // counters below are the whole blocked phase and nothing of the shutdown drain.
        g_flooder = getCoreStats();
        _done     = true;
    }
};

TEST(FlushYieldsToInbound, ABlockedPassTriesTheHeadOnceWhenInboundWorkIsPending) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the flood must cross a mailbox";
    g_flooder  = {};
    g_received = g_in_order = g_steps = 0;

    qb::Main   main;
    const auto sink = main.addActor<Sink>(0);
    main.addActor<Flooder>(1, sink);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    EXPECT_EQ(g_received, kFlood) << "the flood arrives whole: the early bail drops nothing";
    EXPECT_EQ(g_in_order, kFlood) << "and in order: the pipe is FIFO and the bail keeps the head where it was";
    EXPECT_EQ(g_flooder.events_dropped, 0u);
    EXPECT_GT(g_flooder.sends_blocked, 0u) << "the sink's ring must have been found full, or nothing was tested";
    // The first refusals of the episode run the whole budget (the peer may be draining: nothing to compare
    // with yet, then one unmoved reading), and so does a refusal after each batch the sink drained; every
    // other refusal finds the sink's index unmoved and bails at once. Without the rule EVERY blocked pass
    // spends the budget: 513 attempts per pass against this bound's handful per episode.
    EXPECT_LE(g_flooder.sends_blocked, g_flooder.loop_passes + kBudgetBound)
        << "a blocked pass with inbound work pending and a stalled peer tries the head event once; the 512-attempt "
           "backoff is for a core with nothing else to do, or a peer that is draining";
    EXPECT_GT(g_steps, 0u);
}

} // namespace flush_yields_to_inbound_test
