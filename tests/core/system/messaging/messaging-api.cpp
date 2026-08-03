/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/messaging-api.cpp
 * @brief Event delivery across every send primitive and core topology.
 *
 * Proves each of the five ways an actor can emit an event —
 *   push<E>(to) · send<E>(to) · to(id).push<E>() · getPipe(id).push<E>() ·
 *   getPipe(id).allocated_push<E>(n)
 * — delivers the exact payload bytes (checksum-validated, see shared/ChecksumEvent.h) under
 * five topologies: same-core, cross-core ring, cross-core ring with induced latency, and
 * targeted broadcast both mono- and multi-core. Two independent oracles guard every run:
 *   1. per-receiver: its destructor asserts it saw EXACTLY `_max_events` (under-delivery fails);
 *   2. global: a post-`join()` total asserts the engine delivered EXACTLY the expected count.
 *
 * Also folds the engine-level `broadcast<E>(args)` value test (was test-actor-broadcast.cpp:
 * arithmetic-series value sum, not just a count) and adds an explicit `unregisterEvent` test
 * proving a de-registered handler receives nothing.
 */

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <qb/actor.h>
#include <qb/main.h>
#include "../../shared/ChecksumEvent.h"

using qb::test::copyAllocatedPayload;
using qb::test::TestEvent;

// Total TestEvent deliveries observed across all receivers in the current test (reset per run).
static std::atomic<std::uint32_t> g_msg_delivered{0};

struct RemovedEvent : public qb::Event {};

// Receiver: validates each payload, counts deliveries (per-actor + global), self-kills at cap.
class TestActorReceiver final : public qb::Actor {
    const std::uint32_t _max_events;
    std::uint32_t       _count = 0;

public:
    explicit TestActorReceiver(std::uint32_t const max_events)
        : _max_events(max_events) {
        registerEvent<TestEvent>(*this);
        // Register then immediately de-register: no RemovedEvent is ever sent here, so this only
        // documents the API; the de-registration *contract* is asserted in UnregisterStopsDelivery.
        registerEvent<RemovedEvent>(*this);
        unregisterEvent<RemovedEvent>(*this);
    }

    ~TestActorReceiver() final {
        // Oracle 1: this receiver must have seen exactly its quota (no over/under-delivery).
        EXPECT_EQ(_count, _max_events);
    }

    void
    on(TestEvent const &event) {
        EXPECT_TRUE(event.checkSum());
        g_msg_delivered.fetch_add(1, std::memory_order_relaxed);
        if (++_count >= _max_events)
            kill();
    }

    void
    on(RemovedEvent const &) {}
};

// Sender base: emits one event per loop tick via the derived doSend(), exactly _max_events times.
class BaseSender {
public:
    const std::uint32_t _max_events;
    const qb::ActorId   _to;
    std::uint32_t       _count = 0;

    BaseSender(std::uint32_t const max_events, qb::ActorId const to)
        : _max_events(max_events)
        , _to(to) {}
    ~BaseSender() {
        EXPECT_EQ(_count, _max_events);
    }
};

template <typename Derived>
class BaseActorSender
    : public BaseSender
    , public qb::Actor
    , public qb::ICallback {
public:
    BaseActorSender(std::uint32_t const max_events, qb::ActorId const to)
        : BaseSender(max_events, to) {}

    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        static_cast<Derived &>(*this).doSend();
        if (++_count >= _max_events)
            kill();
    }
};

struct BasicPushActor : BaseActorSender<BasicPushActor> {
    using BaseActorSender::BaseActorSender;
    void
    doSend() {
        push<TestEvent>(_to);
    }
};
struct BasicSendActor : BaseActorSender<BasicSendActor> {
    using BaseActorSender::BaseActorSender;
    void
    doSend() {
        send<TestEvent>(_to);
    }
};
struct EventBuilderPushActor : BaseActorSender<EventBuilderPushActor> {
    using BaseActorSender::BaseActorSender;
    void
    doSend() {
        to(_to).push<TestEvent>();
    }
};
struct PipePushActor : BaseActorSender<PipePushActor> {
    using BaseActorSender::BaseActorSender;
    void
    doSend() {
        getPipe(_to).push<TestEvent>();
    }
};
struct AllocatedPipePushActor : BaseActorSender<AllocatedPipePushActor> {
    using BaseActorSender::BaseActorSender;
    void
    doSend() {
        auto &e          = getPipe(_to).allocated_push<TestEvent>(32);
        e.has_extra_data = true;
        copyAllocatedPayload(e);
    }
};

#ifdef NDEBUG
constexpr std::uint32_t MAX_ACTORS = 1024u;
constexpr std::uint32_t MAX_EVENTS = 1024u;
#else
constexpr std::uint32_t MAX_ACTORS = 8u;
constexpr std::uint32_t MAX_EVENTS = 8u;
#endif

/** Cap multi-core system tests so CI and large machines do not spawn excessive cores. */
constexpr std::uint32_t MAX_TEST_CORES = 8u;

[[nodiscard]] inline std::uint32_t
testSystemCoreCount() {
    const unsigned      hw = std::thread::hardware_concurrency();
    const std::uint32_t n  = hw == 0u ? 1u : static_cast<std::uint32_t>(hw);
    return std::min(n, MAX_TEST_CORES);
}

// Base fixture: resets the global delivery counter before every run and exposes the expected
// total so each TEST can assert the precise number of deliveries after join().
class MessagingFixture : public testing::Test {
protected:
    qb::Main main;
    void
    SetUp() override {
        g_msg_delivered.store(0, std::memory_order_relaxed);
    }
    [[nodiscard]] virtual std::uint32_t expected_deliveries() const = 0;

    void
    run_and_verify() {
        main.start();
        main.join();
        EXPECT_FALSE(main.hasError());
        // Oracle 2: the engine delivered exactly the expected number of events, no more, no less.
        EXPECT_EQ(g_msg_delivered.load(std::memory_order_relaxed), expected_deliveries());
    }
};

template <typename ActorSender>
class ActorEventMono : public MessagingFixture {
protected:
    void
    SetUp() override {
        MessagingFixture::SetUp();
        for (auto i = 0u; i < MAX_ACTORS; ++i)
            main.addActor<ActorSender>(0, MAX_EVENTS, main.addActor<TestActorReceiver>(0, MAX_EVENTS));
    }
    std::uint32_t
    expected_deliveries() const override {
        return MAX_ACTORS * MAX_EVENTS;
    }
};

template <typename ActorSender>
class ActorEventMulti : public MessagingFixture {
protected:
    const std::uint32_t max_core = testSystemCoreCount();
    void
    SetUp() override {
        MessagingFixture::SetUp();
        for (auto i = 0u; i < max_core; ++i)
            for (auto j = 0u; j < MAX_ACTORS; ++j)
                main.addActor<ActorSender>(i, MAX_EVENTS, main.addActor<TestActorReceiver>((i + 1) % max_core, MAX_EVENTS));
    }
    std::uint32_t
    expected_deliveries() const override {
        return max_core * MAX_ACTORS * MAX_EVENTS;
    }
};

template <typename ActorSender>
class ActorEventMultiHighLatency : public MessagingFixture {
protected:
    const std::uint32_t max_core = testSystemCoreCount();
    void
    SetUp() override {
        MessagingFixture::SetUp();
        for (auto i = 0u; i < max_core; ++i)
            for (auto j = 0u; j < MAX_ACTORS; ++j) {
                main.core((i + 1) % max_core).setLatency(std::chrono::nanoseconds(100));
                main.addActor<ActorSender>(i, MAX_EVENTS, main.addActor<TestActorReceiver>((i + 1) % max_core, MAX_EVENTS));
            }
    }
    std::uint32_t
    expected_deliveries() const override {
        return max_core * MAX_ACTORS * MAX_EVENTS;
    }
};

template <typename ActorSender>
class ActorEventBroadcastMono : public MessagingFixture {
protected:
    void
    SetUp() override {
        MessagingFixture::SetUp();
        main.addActor<ActorSender>(0, MAX_EVENTS, qb::BroadcastId(0));
        for (auto i = 0u; i < MAX_ACTORS; ++i)
            main.addActor<TestActorReceiver>(0, MAX_EVENTS);
    }
    std::uint32_t
    expected_deliveries() const override {
        return MAX_ACTORS * MAX_EVENTS;
    }
};

template <typename ActorSender>
class ActorEventBroadcastMulti : public MessagingFixture {
protected:
    const std::uint32_t max_core = testSystemCoreCount();
    void
    SetUp() override {
        MessagingFixture::SetUp();
        for (auto i = 0u; i < max_core; ++i) {
            main.addActor<ActorSender>(i, MAX_EVENTS, qb::BroadcastId((i + 1) % max_core));
            for (auto j = 0u; j < MAX_ACTORS; ++j)
                main.addActor<TestActorReceiver>((i + 1) % max_core, MAX_EVENTS);
        }
    }
    std::uint32_t
    expected_deliveries() const override {
        return max_core * MAX_ACTORS * MAX_EVENTS;
    }
};

using Implementations = testing::Types<BasicPushActor, BasicSendActor, EventBuilderPushActor, PipePushActor, AllocatedPipePushActor>;

TYPED_TEST_SUITE(ActorEventMono, Implementations);
TYPED_TEST_SUITE(ActorEventBroadcastMono, Implementations);
TYPED_TEST_SUITE(ActorEventMulti, Implementations);
TYPED_TEST_SUITE(ActorEventBroadcastMulti, Implementations);
TYPED_TEST_SUITE(ActorEventMultiHighLatency, Implementations);

TYPED_TEST(ActorEventMono, SendEvents) {
    this->run_and_verify();
}
TYPED_TEST(ActorEventBroadcastMono, SendEvents) {
    this->run_and_verify();
}

TYPED_TEST(ActorEventMulti, SendEvents) {
    if (this->max_core < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise cross-core delivery";
    this->run_and_verify();
}
TYPED_TEST(ActorEventBroadcastMulti, SendEvents) {
    if (this->max_core < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise cross-core broadcast";
    this->run_and_verify();
}
TYPED_TEST(ActorEventMultiHighLatency, SendEvents) {
    if (this->max_core < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise cross-core latency";
    this->run_and_verify();
}

// --- Engine-level broadcast<E>(args): value delivery (folded from test-actor-broadcast.cpp) ---

struct BroadcastValueEvent : public qb::Event {
    int value;
    explicit BroadcastValueEvent(int v)
        : value(v) {}
};
struct EndBroadcastEvent : public qb::Event {};

static std::atomic<int> g_bcast_count{0};
static std::atomic<int> g_bcast_value_sum{0};

class BroadcastValueReceiver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<BroadcastValueEvent>(*this);
        registerEvent<EndBroadcastEvent>(*this);
        co_return true;
    }
    void
    on(const BroadcastValueEvent &e) {
        g_bcast_count.fetch_add(1, std::memory_order_relaxed);
        g_bcast_value_sum.fetch_add(e.value, std::memory_order_relaxed);
    }
    void
    on(const EndBroadcastEvent &) {
        kill();
    }
};

class BroadcastValueEmitter : public qb::Actor {
    const int _n;

public:
    explicit BroadcastValueEmitter(int n)
        : _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 1; i <= _n; ++i)
            broadcast<BroadcastValueEvent>(i);
        broadcast<EndBroadcastEvent>(); // ordered after the value events: receivers stop having seen all
        kill();
        co_return true;
    }
};

TEST(MessagingBroadcast, ValueDeliveryToAllReceivers) {
    g_bcast_count           = 0;
    g_bcast_value_sum       = 0;
    constexpr int receivers = 5, broadcasts = 10;

    qb::Main main;
    for (int i = 0; i < receivers; ++i)
        main.addActor<BroadcastValueReceiver>(0);
    main.addActor<BroadcastValueEmitter>(0, broadcasts);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bcast_count.load(), receivers * broadcasts);
    // Each receiver must have seen every distinct value 1..10 → sum is the arithmetic series.
    EXPECT_EQ(g_bcast_value_sum.load(), receivers * (broadcasts * (broadcasts + 1)) / 2);
}

TEST(MessagingBroadcast, ZeroBroadcastsDeliverNothing) {
    g_bcast_count           = 0;
    g_bcast_value_sum       = 0;
    constexpr int receivers = 3;

    qb::Main main;
    for (int i = 0; i < receivers; ++i)
        main.addActor<BroadcastValueReceiver>(0);
    main.addActor<BroadcastValueEmitter>(0, 0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bcast_count.load(), 0);
    EXPECT_EQ(g_bcast_value_sum.load(), 0);
}

// --- unregisterEvent: a de-registered handler receives nothing ---

static std::atomic<int> g_unreg_received{0};
static std::atomic<int> g_unreg_control{0};

struct ProbeEvent : public qb::Event {};
struct ControlTick : public qb::Event {};

// Registers ProbeEvent then unregisters it before any is sent; a later ProbeEvent must be dropped.
// A ControlTick (still registered) confirms the actor is alive and processing its mailbox, so a
// zero ProbeEvent count means "de-registered", not "never scheduled".
class UnregisterProbeActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ProbeEvent>(*this);
        unregisterEvent<ProbeEvent>(*this);
        registerEvent<ControlTick>(*this);
        co_return true;
    }
    void
    on(const ProbeEvent &) {
        g_unreg_received.fetch_add(1, std::memory_order_relaxed);
    }
    void
    on(const ControlTick &) {
        g_unreg_control.fetch_add(1, std::memory_order_relaxed);
        kill();
    }
};

class UnregisterDriverActor : public qb::Actor {
    const qb::ActorId _probe;

public:
    explicit UnregisterDriverActor(qb::ActorId probe)
        : _probe(probe) {}
    qb::io::async::task<bool>
    onInit() override {
        push<ProbeEvent>(_probe);  // must be dropped (handler de-registered)
        push<ControlTick>(_probe); // must be handled (proves liveness + ordering)
        kill();
        co_return true;
    }
};

TEST(MessagingUnregister, UnregisterStopsDelivery) {
    g_unreg_received = 0;
    g_unreg_control  = 0;

    qb::Main main;
    auto     probe = main.addActor<UnregisterProbeActor>(0);
    main.addActor<UnregisterDriverActor>(0, probe);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_unreg_control.load(), 1);  // the actor ran and drained its mailbox
    EXPECT_EQ(g_unreg_received.load(), 0); // ...yet the de-registered ProbeEvent was never delivered
}
