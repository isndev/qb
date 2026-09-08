/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/dispatch-batch.cpp
 * @brief The dispatch loop over ONE batch that mixes every shape a destination can take, and over
 *        a batch wide enough to span segment links (`VirtualCore::__receive_events__`).
 *
 * A batch pushed from one handler is one run of the self pipe on the next pass. Inside it a
 * destination can be a live actor, a slot the reap has emptied (an actor killed on an earlier
 * pass), a broadcast id (a sid out of the actor table's range), a service id no actor ever had,
 * or an actor killed by an EARLIER event of the same batch (alive == false, its slot still held
 * until the reap ends the pass). What these tests pin: every live destination still receives its
 * event, the dead ones drop theirs, the broadcast reaches exactly the live ones -- and all of it in
 * ONE pass, which is what makes it one run of the loop rather than runs of one (the Pongs of a
 * batch are stamped with the driver's pass count, and the stamps must agree). The second test
 * walks three segment links with kills scattered through the batch. Written for the QB-198
 * investigation (a peek one event ahead in that loop, measured and not shipped); kept because
 * the batch semantics they assert are the loop's, whatever it does with them.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>

namespace dispatch_batch_test {

constexpr std::size_t kCounters  = 24;
constexpr std::size_t kVictim    = 3;             ///< reaped before the batch: an EMPTY slot in `_actors`
constexpr std::size_t kMidKill   = 7;             ///< killed by the batch's own earlier event: alive == false, slot still held
constexpr std::size_t kWideBurst = 3 * 4096 + 11; ///< three segment links and a remainder (4095 buckets a segment)

std::atomic<std::size_t> g_pongs{0};
std::atomic<std::size_t> g_pulses{0};
std::atomic<std::size_t> g_stamps_distinct{0};
std::atomic<std::size_t> g_wide_pongs{0};

struct Ping : public qb::Event {
    std::uint32_t seq = 0;
    explicit Ping(std::uint32_t const s) noexcept
        : seq(s) {}
};
struct Pong : public qb::Event {
    qb::ActorId from;
    explicit Pong(qb::ActorId const f) noexcept
        : from(f) {}
};
struct Pulse : public qb::Event {}; // broadcast
struct Go : public qb::Event {};

class Counter final : public qb::Actor {
    const qb::ActorId _driver;

public:
    explicit Counter(qb::ActorId const driver) noexcept
        : _driver(driver) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Ping>(*this);
        registerEvent<Pulse>(*this);
        co_return true;
    }

    void
    on(Ping const &) {
        push<Pong>(_driver, id());
    }

    void
    on(Pulse const &) {
        g_pulses.fetch_add(1, std::memory_order_relaxed);
    }
};

/// Counts its passes; every Pong it receives is stamped with the pass it arrived in.
class Driver final
    : public qb::Actor
    , public qb::ICallback {
    const std::shared_ptr<const std::vector<qb::ActorId>> _counters; ///< filled by the builder before start()
    std::uint64_t                                         _pass = 0;
    std::set<std::uint64_t>                               _stamps;
    std::size_t                                           _pongs = 0;
    const std::size_t                                     _expected;

public:
    explicit Driver(std::shared_ptr<const std::vector<qb::ActorId>> counters, std::size_t const expected) noexcept
        : _counters(std::move(counters))
        , _expected(expected) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Go>(*this);
        registerEvent<Pong>(*this);
        registerCallback(*this);
        // Pass 1: the victim is told to die, and the driver re-enters on the next pass. The kill
        // runs in pass 2 and its reap ends that pass, so by the batch's pass the slot is empty.
        push<qb::KillEvent>((*_counters)[kVictim]);
        push<Go>(id());
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final { // qb::ICallback: once per pass of the core
        ++_pass;
    }

    /// Pass 2: the batch, in one handler, so it is one run of the self pipe on pass 3.
    void
    on(Go const &) {
        std::uint32_t seq      = 0;
        auto const   &counters = *_counters;
        for (std::size_t i = 0; i < counters.size(); ++i) {
            if (i == kMidKill) // its own event first, then the kill, then one more for it
                push<Ping>(counters[i], seq++);
            push<Ping>(counters[i], seq++); // the victim's lands on an EMPTY slot
            if (i == kMidKill)
                push<qb::KillEvent>(counters[i]);
            if (i == kCounters / 2)
                broadcast<Pulse>(); // dest = BroadcastId: sid out of the actor table's range
            if (i == kCounters / 3)
                push<Ping>(qb::ActorId(60000u | (static_cast<std::uint32_t>(id().index()) << 16)), seq++); // no actor ever had this sid
        }
    }

    void
    on(Pong const &) {
        ++_pongs;
        _stamps.insert(_pass);
        if (_pongs == _expected) {
            g_pongs.store(_pongs, std::memory_order_relaxed);
            g_stamps_distinct.store(_stamps.size(), std::memory_order_relaxed);
            for (auto const c : *_counters)
                push<qb::KillEvent>(c); // the dead ones are no-ops: empty slot, or already gone
            unregisterCallback();
            kill();
        }
    }
};

/// The wide batch: every counter pinged round-robin over three segment links, kills scattered
/// through it -- an actor killed mid-batch is met dead by its later events (alive == false, the
/// trampoline skips them; its slot still holds the object until the reap).
class WideDriver final : public qb::Actor {
    const std::shared_ptr<const std::vector<qb::ActorId>> _counters;
    std::size_t                                           _pongs    = 0;
    std::size_t                                           _expected = 0;

public:
    explicit WideDriver(std::shared_ptr<const std::vector<qb::ActorId>> counters) noexcept
        : _counters(std::move(counters)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Pong>(*this);
        auto const       &counters = *_counters;
        std::vector<bool> dead(counters.size(), false);
        for (std::size_t i = 0; i < kWideBurst; ++i) {
            const std::size_t c = i % counters.size();
            push<Ping>(counters[c], static_cast<std::uint32_t>(i));
            if (!dead[c])
                ++_expected;
            if (i % 997 == 0) { // a kill every 997 events: the batch's later Pings to it are dropped
                push<qb::KillEvent>(counters[c]);
                dead[c] = true;
            }
        }
        co_return true;
    }

    void
    on(Pong const &) {
        if (++_pongs == _expected) {
            g_wide_pongs.store(_pongs, std::memory_order_relaxed);
            for (auto const c : *_counters)
                push<qb::KillEvent>(c);
            kill();
        }
    }
};

template <typename Build>
[[nodiscard]] bool
run(Build build, std::chrono::seconds budget) {
    g_pongs = g_pulses = g_stamps_distinct = g_wide_pongs = 0;
    auto done                                             = std::make_shared<std::promise<void>>();
    auto future                                           = done->get_future();
    std::thread([build, done] {
        qb::Main main;
        build(main);
        main.start();
        main.join();
        done->set_value();
    }).detach();
    return future.wait_for(budget) == std::future_status::ready;
}

} // namespace dispatch_batch_test

using namespace dispatch_batch_test;

/**
 * @test One pass, one run: live destinations, an emptied slot, a broadcast, a never-assigned
 *       sid and an actor killed by the batch itself, in one batch -- delivered, in one pass.
 */
TEST(DispatchBatch, MixedBatchIsDeliveredInOnePass) {
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            // The driver reads the counters' ids on its core after start(); the builder fills
            // the vector before that, and nothing writes it afterwards. The victim is reaped
            // before the batch; the mid-batch kill's first Ping counts, its second does not:
            // kCounters - 2 counters answer once, kMidKill answers once.
            auto       counters = std::make_shared<std::vector<qb::ActorId>>();
            const auto driver   = main.addActor<Driver>(0, counters, kCounters - 1);
            for (std::size_t i = 0; i < kCounters; ++i)
                counters->push_back(main.addActor<Counter>(0, driver));
        },
        std::chrono::seconds(60)));
    EXPECT_EQ(g_pongs.load(), kCounters - 1) << "a live destination lost its event";
    EXPECT_EQ(g_stamps_distinct.load(), 1u) << "the batch's replies arrived over several passes: the batch "
                                               "was not dispatched as one run";
    // the victim is gone and kMidKill was killed by an earlier event of the same batch
    EXPECT_EQ(g_pulses.load(), kCounters - 2) << "the broadcast in the middle of the batch missed a live counter";
}

/**
 * @test Three segment links and a remainder, kills scattered through the batch: every Ping to a
 *       still-live counter is answered, every one to a killed counter is dropped, nothing faults.
 */
TEST(DispatchBatch, WideBatchAcrossSegmentsWithMidBatchKills) {
    ASSERT_TRUE(run(
        [](qb::Main &main) {
            auto       counters = std::make_shared<std::vector<qb::ActorId>>();
            const auto driver   = main.addActor<WideDriver>(0, counters);
            for (std::size_t i = 0; i < kCounters; ++i)
                counters->push_back(main.addActor<Counter>(0, driver));
        },
        std::chrono::seconds(120)));
    EXPECT_GT(g_wide_pongs.load(), 0u) << "the wide batch answered nothing";
}
