/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/pubsub-dead-subscribers.cpp
 * @brief `qb::PubSub<Topic>` must not accumulate subscribers that are already dead.
 *
 * The bus keeps a plain `std::vector<qb::ActorId>` and only ever removes an entry through an
 * explicit `unsubscribe()`. A subscriber that is simply killed — the normal way an actor ends —
 * leaves its id behind forever, and every later `publish()` still constructs a `Topic` event for
 * it, routes it, finds no handler and disposes it.
 *
 * That matters because the framework prunes its *own* equivalent map: `VirtualCore::removeActor`
 * calls `unregisterEvents(id)`, which drops the actor from every event's handler map in the
 * router. A user-space mirror of that map with no such hook grows without bound in any system
 * whose subscribers churn (a bus per connection, per session, per request), and each publish pays
 * for every corpse.
 *
 * This pins the behaviour a bus owes its users: after a subscriber dies, it must stop being
 * counted and stop being published to.
 */

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <gtest/gtest.h>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/core/patterns/pubsub.h>

namespace {

constexpr std::size_t kSubscribers = 8; ///< half are killed before the publish

std::atomic<int>         g_delivered{0};
std::atomic<std::size_t> g_count_after_deaths{0};

struct Tick : public qb::Event {
    int seq;
    explicit Tick(int s)
        : seq(s) {}
};

/// Subscribes on init; the odd-indexed ones kill themselves as soon as they are told to.
class Sub final : public qb::Actor {
    const bool _dies;

public:
    explicit Sub(bool dies)
        : _dies(dies) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        this->getService<qb::PubSub<Tick>>()->subscribe(id());
        if (_dies)
            kill(); // dies WITHOUT unsubscribing — the ordinary actor ending
        co_return true;
    }
    void
    on(Tick const &) {
        g_delivered.fetch_add(1, std::memory_order_relaxed);
    }
};

/// Runs one loop pass after the deaths, then publishes and records what the bus believes.
class Publisher final
    : public qb::Actor
    , public qb::ICallback {
    int _tick = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        ++_tick;
        if (_tick == 3) { // give the reap phase time to destroy the killed subscribers
            auto *bus = this->getService<qb::PubSub<Tick>>();
            g_count_after_deaths.store(bus->subscriber_count(), std::memory_order_relaxed);
            bus->publish(42);
        } else if (_tick > 6) {
            // The bus is a ServiceActor and never kills itself, so the core would otherwise
            // never empty `_actors`. Stop the engine the way the other pubsub tests do.
            qb::Main::stop();
        }
    }
};

} // namespace

TEST(PubSubDeadSubscribers, KilledSubscribersAreNeitherCountedNorPublishedTo) {
    g_delivered          = 0;
    g_count_after_deaths = 0;

    {
        qb::Main main;
        main.addActor<qb::PubSub<Tick>>(0);
        for (std::size_t i = 0; i < kSubscribers; ++i)
            main.addActor<Sub>(0, /*dies=*/(i % 2) == 1);
        main.addActor<Publisher>(0);
        main.start(false);
        main.join();
    }

    const auto expected_live = static_cast<std::size_t>(kSubscribers / 2);

    EXPECT_EQ(g_count_after_deaths.load(), expected_live)
        << "the bus still counts subscribers that have already been destroyed: its vector only "
           "shrinks on an explicit unsubscribe(), so a killed subscriber leaks its slot forever "
           "and every publish keeps building a Topic event for it";
    EXPECT_EQ(g_delivered.load(), static_cast<int>(expected_live)) << "only live subscribers may receive a publication";
}

// ---------------------------------------------------------------------------------------------
// The invariant the prune actually exists for — and why the obvious test for it is degenerate.
//
// The first test passes even with EVERY prune removed: `subscriber_count()` filters by liveness,
// and a dead id cannot receive anything because the router finds no handler for it. So it pins
// correctness, not boundedness.
//
// The obvious boundedness test — churn generations of subscribers and watch the list grow — was
// ALSO degenerate with 32-bit ids, and it took a measurement to see why: `VirtualCore` recycled
// actor ids (the smallest free one), so a subscriber that died freed its id for the next one, which
// then hit `subscribe()`'s duplicate check and never grew the list. Measured peak with and without
// the prune: 1 slot over 64 generations, identical. Since 3.3 a recycled slot comes back under a
// new generation (QB-970): the duplicate check misses, and the prune is what bounds the list.
//
// What this pins instead is exactly what the design guarantees: `_subscribers` only grows in
// `subscribe()` — pruning on every publish cost a measured 10-14% of dispatch throughput and was
// moved out — so a later `subscribe()` must reclaim the slots of subscribers that died while
// others stayed alive (their ids are NOT recycled, since the survivors still hold theirs).
// ---------------------------------------------------------------------------------------------

namespace {

std::atomic<std::size_t> g_slots_before{0};
std::atomic<std::size_t> g_slots_after{0};
std::vector<qb::ActorId> g_reclaim_ids;

/// Subscribes and STAYS ALIVE through init, so every id is in the list before any death.
class StableSub final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        this->getService<qb::PubSub<Tick>>()->subscribe(id());
        g_reclaim_ids.push_back(id());
        co_return true;
    }
    void
    on(Tick const &) {}
};

class LateSub final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        this->getService<qb::PubSub<Tick>>()->subscribe(id());
        co_return true;
    }
    void
    on(Tick const &) {}
};

/// tick 1: kill half. tick 3: record slots (deaths reaped, nothing subscribed since).
/// tick 4: subscribe one more — the growth point, which must reclaim. tick 6: record again.
class ReclaimDriver final
    : public qb::Actor
    , public qb::ICallback {
    int _tick = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        auto *bus = this->getService<qb::PubSub<Tick>>();
        ++_tick;
        if (_tick == 1) {
            for (std::size_t i = 1; i < g_reclaim_ids.size(); i += 2)
                this->template push<qb::KillEvent>(g_reclaim_ids[i]);
        } else if (_tick == 3) {
            g_slots_before.store(bus->tracked_slot_count(), std::memory_order_relaxed);
        } else if (_tick == 4) {
            this->template addRefActor<LateSub>(); // the growth point — must prune first
        } else if (_tick == 6) {
            g_slots_after.store(bus->tracked_slot_count(), std::memory_order_relaxed);
        } else if (_tick > 7) {
            qb::Main::stop();
        }
    }
};

} // namespace

TEST(PubSubDeadSubscribers, ALaterSubscribeReclaimsDeadSlots) {
    g_slots_before.store(0, std::memory_order_relaxed);
    g_slots_after.store(0, std::memory_order_relaxed);
    g_reclaim_ids.clear();

    {
        qb::Main main;
        main.addActor<qb::PubSub<Tick>>(0);
        for (std::size_t i = 0; i < kSubscribers; ++i)
            main.addActor<StableSub>(0);
        main.addActor<ReclaimDriver>(0);
        main.start(false);
        main.join();
    }

    const auto live = static_cast<std::size_t>(kSubscribers / 2);

    ASSERT_EQ(g_slots_before.load(), kSubscribers)
        << "precondition: all " << kSubscribers << " ids are in the list, and nothing has subscribed since the deaths";
    EXPECT_EQ(g_slots_after.load(), live + 1) << "after a new subscribe the bus holds " << g_slots_after.load() << " ids instead of "
                                              << (live + 1) << " (the " << live
                                              << " survivors plus the newcomer): `subscribe()` is not reclaiming the slots of "
                                                 "subscribers that died, so the list grows without bound as subscribers churn";
}

namespace pubsub_service_address_test {

struct SubscriberTag {};
std::atomic<int>         g_service_constructed{0};
std::atomic<int>         g_service_destroyed{0};
std::atomic<int>         g_service_delivered{0};
std::atomic<bool>        g_logical_alive{false};
std::atomic<bool>        g_old_instance_dead{false};
std::atomic<bool>        g_replacement_alive{false};
std::atomic<bool>        g_replacement_distinct{false};
std::atomic<std::size_t> g_count_initial{0};
std::atomic<std::size_t> g_count_after_prune{0};
std::atomic<std::size_t> g_slots_after_prune{0};
std::atomic<std::size_t> g_count_after_replacement{0};

class ServiceSubscriber : public qb::ServiceActor<SubscriberTag> {
public:
    ServiceSubscriber() {
        ++g_service_constructed;
    }
    ~ServiceSubscriber() override {
        ++g_service_destroyed;
    }
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Tick>(*this);
        co_return true;
    }
    void
    on(Tick const &) {
        ++g_service_delivered;
    }
};

class Driver
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _original;
    int         _pass = 0;

public:
    explicit Driver(qb::ActorId original)
        : _original(original) {}
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Tick>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(Tick const &) {}
    void
    on(qb::LoopEvent const &) final {
        ++_pass;
        if (_pass == 1) {
            const auto address = getServiceId<SubscriberTag>(getIndex());
            auto      *bus     = getService<qb::PubSub<Tick>>();
            bus->subscribe(address);
            g_logical_alive.store(is_actor_alive(address));
            g_count_initial.store(bus->subscriber_count());
            bus->subscribe(id()); // a second subscribe invokes pruning of the logical service
            g_count_after_prune.store(bus->subscriber_count());
            g_slots_after_prune.store(bus->tracked_slot_count());
            bus->publish(17);
        } else if (_pass == 3) {
            push<qb::KillEvent>(_original);
        } else if (_pass == 5) {
            const auto replacement = addRefActor<ServiceSubscriber>();
            g_replacement_distinct.store(replacement.valid() && replacement.id() != _original && replacement.id().slot() == _original.slot());
            g_old_instance_dead.store(!is_actor_alive(_original));
            g_replacement_alive.store(replacement.valid() && is_actor_alive(replacement.id())
                                      && is_actor_alive(getServiceId<SubscriberTag>(getIndex())));
            auto *bus = getService<qb::PubSub<Tick>>();
            g_count_after_replacement.store(bus->subscriber_count());
            bus->publish(99); // the retained logical subscription follows this instance
        } else if (_pass == 8) {
            push<qb::KillEvent>(qb::BroadcastId(getIndex()));
        }
    }
};

} // namespace pubsub_service_address_test

TEST(PubSubDeadSubscribers, LogicalServiceSubscriberSurvivesPruningAndFollowsTheCurrentInstance) {
    using namespace pubsub_service_address_test;
    g_service_constructed.store(0);
    g_service_destroyed.store(0);
    g_service_delivered.store(0);
    g_logical_alive.store(false);
    g_old_instance_dead.store(false);
    g_replacement_alive.store(false);
    g_replacement_distinct.store(false);
    g_count_initial.store(0);
    g_count_after_prune.store(0);
    g_slots_after_prune.store(0);
    g_count_after_replacement.store(0);
    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    const auto original = main.addActor<ServiceSubscriber>(0);
    ASSERT_TRUE(original.is_service_instance());
    main.addActor<Driver>(0, original);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());
    EXPECT_EQ(g_service_constructed.load(), 2);
    EXPECT_EQ(g_service_destroyed.load(), 2);
    EXPECT_TRUE(g_logical_alive.load());
    EXPECT_EQ(g_count_initial.load(), 1u);
    EXPECT_EQ(g_count_after_prune.load(), 2u);
    EXPECT_EQ(g_slots_after_prune.load(), 2u);
    EXPECT_TRUE(g_replacement_distinct.load());
    EXPECT_TRUE(g_old_instance_dead.load());
    EXPECT_TRUE(g_replacement_alive.load());
    EXPECT_EQ(g_count_after_replacement.load(), 2u);
    EXPECT_EQ(g_service_delivered.load(), 2) << "both service instances must receive the retained logical subscription";
}
