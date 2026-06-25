/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/pubsub-fanout.cpp
 * @brief `qb::PubSub<Topic>` — the per-core publish/subscribe bus, under the live engine.
 *
 * `PubSub<Topic>` is a per-`VirtualCore` `ServiceActor`: subscribers call `subscribe(id())` and
 * register a `Topic` handler; a publisher calls `publish(args…)` to build one `Topic` per
 * subscriber and `push` it out. This SYSTEM file proves the fan-out, unsubscribe, count, and
 * async-subscribe contracts end-to-end.
 *
 * What is proven:
 *   - `FanOutReachesAllSubscribers`: one publish lands one copy of the exact payload on every
 *     current subscriber (value-checked, not just counted);
 *   - `UnsubscribeStopsDelivery`: an actor that subscribes-then-leaves receives nothing while a
 *     staying subscriber still gets every publication;
 *   - `UnsubscribeMidPublishStopsThatSubscriber`: removing one subscriber *between* two
 *     publications drops it from the second wave only — the others keep receiving (the subscriber
 *     list is read live, per-publish);
 *   - `PublishToZeroSubscribersIsANoOp`: publishing on an empty bus delivers nothing and the
 *     engine still shuts down cleanly (no crash, no hang, subscriber_count stays 0);
 *   - `SubscriberCountTracksSubscribeUnsubscribe`: `subscribe` is idempotent (double-subscribe
 *     still counts 1), `unsubscribe` decrements;
 *   - `SubscriberSubscribingInAsyncOnInitReceivesPublish`: a subscriber that joins only after an
 *     async suspension still receives a later publication.
 *
 * DE-FLAKE: the publisher does NOT sleep a fixed 20ms hoping every subscriber has registered.
 * Instead every subscriber pushes a `SubReady` (carrying its own id) to the publisher the instant
 * it has registered + subscribed; the publisher publishes only once it has counted all expected
 * `SubReady`s. The publish therefore happens-after every subscribe by construction,
 * deterministically — the ctest TIMEOUT is the only backstop, never a wall-clock EXPECT.
 *
 * Every in-actor `EXPECT_*` / count is paired with a post-`join()` `ran` guard so a
 * never-scheduled actor cannot make a test pass vacuously.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor suites.
 */

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

// The published topic: carries a value so receivers prove they got the *payload*, not just an
// event of the right type.
struct Tick : public qb::Event {
    int value{0};
    explicit Tick(int v)
        : value(v) {}
};

// A subscriber's signal to the publisher: "I have registered + subscribed; here is my id."
struct SubReady : public qb::Event {
    qb::ActorId who;
    explicit SubReady(qb::ActorId w)
        : who(w) {}
};

namespace {

constexpr int kSubscribers = 3;

std::atomic<int>  g_sub_recv[kSubscribers];      // per-subscriber Tick count
std::atomic<int>  g_sub_value_sum[kSubscribers]; // per-subscriber sum of received Tick values
std::atomic<bool> g_unsub_received{false};       // an actor that left must never receive
std::atomic<int>  g_async_sub_recv{0};           // subscriber that joins in async onInit
std::atomic<int>  g_count_after_sub{-1};
std::atomic<int>  g_count_after_unsub{-1};
std::atomic<int>  g_zero_pub_count{-1};   // subscriber_count seen by the empty-bus publisher
std::atomic<bool> g_publisher_ran{false}; // set after the publisher has published (ran-guard)
std::atomic<bool> g_probe_ran{false};     // set after the count-probe has reported (ran-guard)

void
reset_counters() {
    for (int i = 0; i < kSubscribers; ++i) {
        g_sub_recv[i].store(0, std::memory_order_relaxed);
        g_sub_value_sum[i].store(0, std::memory_order_relaxed);
    }
    g_unsub_received.store(false, std::memory_order_relaxed);
    g_async_sub_recv.store(0, std::memory_order_relaxed);
    g_count_after_sub.store(-1, std::memory_order_relaxed);
    g_count_after_unsub.store(-1, std::memory_order_relaxed);
    g_zero_pub_count.store(-1, std::memory_order_relaxed);
    g_publisher_ran.store(false, std::memory_order_relaxed);
    g_probe_ran.store(false, std::memory_order_relaxed);
}

// Expected sum of values 1..n (arithmetic series) — used to prove every wave's payload arrived.
constexpr int
series_sum(int n) {
    return n * (n + 1) / 2;
}

} // namespace

// A subscriber that registers, subscribes, then reports readiness to the publisher.
class Sub : public qb::Actor {
    const int         _idx;
    const qb::ActorId _publisher;

public:
    Sub(int idx, qb::ActorId publisher)
        : _idx(idx)
        , _publisher(publisher) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        getService<qb::PubSub<Tick>>()->subscribe(id());
        push<SubReady>(_publisher, id()); // gate the publisher on readiness (no sleep)
        co_return true;
    }
    void
    on(Tick &t) {
        g_sub_recv[_idx].fetch_add(1, std::memory_order_relaxed);
        g_sub_value_sum[_idx].fetch_add(t.value, std::memory_order_relaxed);
    }
};

// Subscribes then immediately unsubscribes — must never receive a publication. Still reports
// readiness so the publisher waits for it (proving the leave took effect before the publish).
class UnsubSub : public qb::Actor {
    const qb::ActorId _publisher;

public:
    explicit UnsubSub(qb::ActorId publisher)
        : _publisher(publisher) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        auto *bus = getService<qb::PubSub<Tick>>();
        bus->subscribe(id());
        bus->unsubscribe(id());
        push<SubReady>(_publisher, id());
        co_return true;
    }
    void
    on(Tick &) {
        g_unsub_received.store(true, std::memory_order_relaxed);
    }
};

// Publishes `_publications` Ticks once `_expected_ready` SubReady signals have arrived, then stops
// the engine. The fan-out is therefore strictly after every subscriber has subscribed.
class Publisher : public qb::Actor {
    const int _expected_ready;
    const int _publications;
    int       _ready = 0;

public:
    Publisher(int expected_ready, int publications)
        : _expected_ready(expected_ready)
        , _publications(publications) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SubReady>(*this);
        if (_expected_ready == 0) {
            // No subscribers to wait for: publish into the void immediately.
            auto *bus = getService<qb::PubSub<Tick>>();
            g_zero_pub_count.store(static_cast<int>(bus->subscriber_count()), std::memory_order_relaxed);
            do_publish();
        }
        co_return true;
    }
    void
    on(SubReady &) {
        if (++_ready == _expected_ready)
            do_publish();
    }

private:
    void
    do_publish() {
        auto *bus = getService<qb::PubSub<Tick>>();
        for (int i = 1; i <= _publications; ++i)
            bus->publish(i); // builds Tick{i}
        g_publisher_ran.store(true, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

// ===========================================================================
// Fan-out: one publication reaches every current subscriber.
// ===========================================================================
TEST(PubSubFanout, FanOutReachesAllSubscribers) {
    reset_counters();
    constexpr int publications = 2;

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0); // the bus first
    auto publisher = main.addActor<Publisher>(0, kSubscribers, publications);
    for (int i = 0; i < kSubscribers; ++i)
        main.addActor<Sub>(0, i, publisher);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_publisher_ran.load()) << "publisher must have fired after all subscribers were ready";
    for (int i = 0; i < kSubscribers; ++i) {
        EXPECT_EQ(g_sub_recv[i].load(), publications) << "subscriber " << i << " missed a publication";
        // Values 1..publications were delivered intact, not merely counted.
        EXPECT_EQ(g_sub_value_sum[i].load(), series_sum(publications)) << "subscriber " << i;
    }
}

// ===========================================================================
// Unsubscribe before publish: the leaver gets nothing; the stayer gets everything.
// ===========================================================================
TEST(PubSubFanout, UnsubscribeStopsDelivery) {
    reset_counters();
    constexpr int publications = 2;

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    // The publisher waits for BOTH the stayer and the leaver to report ready, so the publish
    // happens strictly after the unsubscribe — no timing assumption.
    auto publisher = main.addActor<Publisher>(0, /*expected_ready=*/2, publications);
    main.addActor<Sub>(0, 0, publisher);   // stays subscribed
    main.addActor<UnsubSub>(0, publisher); // leaves immediately

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_publisher_ran.load());
    EXPECT_EQ(g_sub_recv[0].load(), publications); // the stayer gets both
    EXPECT_EQ(g_sub_value_sum[0].load(), series_sum(publications));
    EXPECT_FALSE(g_unsub_received.load()) << "an unsubscribed actor must receive nothing";
}

// ===========================================================================
// Unsubscribe mid-stream: removing a subscriber between two publishes drops it from the 2nd only.
// (Missing case: the subscriber list is read live on each publish.)
// ===========================================================================

// Waits for all subscribers to report ready (learning their ids from SubReady), then:
//   wave 1: publish to everyone; unsubscribe the first reporter; wave 2: publish to the rest.
class MidPublishPublisher : public qb::Actor {
    const int   _expected_ready;
    int         _ready  = 0;
    qb::ActorId _victim = {};

public:
    explicit MidPublishPublisher(int expected_ready)
        : _expected_ready(expected_ready) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SubReady>(*this);
        co_return true;
    }
    void
    on(SubReady &e) {
        if (_ready == 0)
            _victim = e.who; // the first subscriber to report becomes the mid-stream victim
        if (++_ready != _expected_ready)
            return;
        auto *bus = getService<qb::PubSub<Tick>>();
        EXPECT_EQ(bus->subscriber_count(), static_cast<std::size_t>(_expected_ready));
        bus->publish(1);           // wave 1: everyone, including the victim
        bus->unsubscribe(_victim); // remove the victim between waves
        EXPECT_EQ(bus->subscriber_count(), static_cast<std::size_t>(_expected_ready - 1));
        bus->publish(2); // wave 2: everyone EXCEPT the victim
        g_publisher_ran.store(true, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(PubSubFanout, UnsubscribeMidPublishStopsThatSubscriber) {
    reset_counters();

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    auto publisher = main.addActor<MidPublishPublisher>(0, kSubscribers);
    for (int i = 0; i < kSubscribers; ++i)
        main.addActor<Sub>(0, i, publisher);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_publisher_ran.load()) << "publisher must have run both waves";

    // Exactly one subscriber (the victim) saw only wave 1 (value 1); the other two saw both
    // waves (values 1 and 2). We do not know which index was first-to-report, so assert the
    // aggregate shape: total deliveries = 1 (victim, wave 1) + 2*2 (others, both waves) = 5,
    // and exactly one subscriber received a single Tick of value 1.
    int total_recv = 0, total_value = 0, victims = 0;
    for (int i = 0; i < kSubscribers; ++i) {
        const int recv = g_sub_recv[i].load();
        const int sum  = g_sub_value_sum[i].load();
        total_recv += recv;
        total_value += sum;
        if (recv == 1) {
            ++victims;
            EXPECT_EQ(sum, 1) << "the victim must have seen only wave-1 value 1, subscriber " << i;
        } else {
            EXPECT_EQ(recv, 2) << "a staying subscriber must have seen both waves, subscriber " << i;
            EXPECT_EQ(sum, series_sum(2)) << "a staying subscriber must have values 1+2, subscriber " << i;
        }
    }
    EXPECT_EQ(victims, 1) << "exactly one subscriber was unsubscribed mid-stream";
    EXPECT_EQ(total_recv, 1 + 2 * (kSubscribers - 1)); // 1 + 4 = 5 deliveries total
    EXPECT_EQ(total_value, 1 + (kSubscribers - 1) * series_sum(2));
}

// ===========================================================================
// Publish to zero subscribers: a clean no-op (no crash, no hang, count stays 0).
// ===========================================================================
TEST(PubSubFanout, PublishToZeroSubscribersIsANoOp) {
    reset_counters();

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    main.addActor<Publisher>(0, /*expected_ready=*/0, /*publications=*/3); // publishes into the void

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_publisher_ran.load()) << "the empty-bus publisher must have run and stopped the engine";
    EXPECT_EQ(g_zero_pub_count.load(), 0) << "the bus must report zero subscribers";
    // No subscribers exist, so no per-subscriber counter could have moved.
    for (int i = 0; i < kSubscribers; ++i)
        EXPECT_EQ(g_sub_recv[i].load(), 0);
}

// ===========================================================================
// subscriber_count tracks subscribe (idempotent) / unsubscribe.
// ===========================================================================

// Subscribes twice (idempotent) then unsubscribes, recording the bus count after each step.
class CountProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto *bus = getService<qb::PubSub<Tick>>();
        EXPECT_EQ(bus->subscriber_count(), 0u); // fresh bus starts empty
        bus->subscribe(id());
        bus->subscribe(id()); // idempotent — still one subscriber
        g_count_after_sub.store(static_cast<int>(bus->subscriber_count()), std::memory_order_relaxed);
        bus->unsubscribe(id());
        g_count_after_unsub.store(static_cast<int>(bus->subscriber_count()), std::memory_order_relaxed);
        bus->unsubscribe(id()); // unsubscribing again is a no-op
        EXPECT_EQ(bus->subscriber_count(), 0u);
        g_probe_ran.store(true, std::memory_order_relaxed);
        qb::Main::stop();
        co_return true;
    }
};

TEST(PubSubFanout, SubscriberCountTracksSubscribeUnsubscribe) {
    reset_counters();

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    main.addActor<CountProbe>(0);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_probe_ran.load()) << "CountProbe::onInit must have run";
    EXPECT_EQ(g_count_after_sub.load(), 1);   // idempotent subscribe -> still 1
    EXPECT_EQ(g_count_after_unsub.load(), 0); // removed -> 0
}

// ===========================================================================
// Distribution x ASYNC init: a subscriber that joins only after a suspension still gets a
// later publication. Gated event-driven: the subscriber reports ready *after* it resumes and
// subscribes, and the publisher only publishes on that signal.
// ===========================================================================

// Subscribes to the bus only after an async suspension, then reports readiness.
class AsyncSub : public qb::Actor {
    const qb::ActorId _publisher;

public:
    explicit AsyncSub(qb::ActorId publisher)
        : _publisher(publisher) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(20ms); // resume later, mid-run
        getService<qb::PubSub<Tick>>()->subscribe(id());
        push<SubReady>(_publisher, id()); // only NOW is the subscription live
        co_return true;
    }
    void
    on(Tick &) {
        g_async_sub_recv.fetch_add(1, std::memory_order_relaxed);
    }
};

TEST(PubSubFanout, SubscriberSubscribingInAsyncOnInitReceivesPublish) {
    reset_counters();

    qb::Main main;
    main.addActor<qb::PubSub<Tick>>(0);
    // The publisher waits for the async subscriber's readiness, which only fires after the
    // subscriber resumes from its sleep and subscribes — so the publish is provably after.
    auto publisher = main.addActor<Publisher>(0, /*expected_ready=*/1, /*publications=*/1);
    main.addActor<AsyncSub>(0, publisher);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_publisher_ran.load());
    EXPECT_EQ(g_async_sub_recv.load(), 1) << "the publish must reach a subscriber that joined in async onInit";
}
