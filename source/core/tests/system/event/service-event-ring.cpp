/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/event/service-event-ring.cpp
 * @brief `ServiceActor<Tag>` cross-core ring delivery, across all five send primitives.
 *
 * One `ServiceActor<MyTag>` per core forms a token ring: actor on core i targets the same service on
 * core (i+1) % N (resolved by `getServiceId<MyTag>(core)`). The core-0 actor injects ONE
 * checksum-validated `TestEvent` in `onInit()`; each receiver validates the exact payload bytes (see
 * shared/ChecksumEvent.h), forwards to the next core (unless it is the core-0 origin, which closes
 * the ring), and self-kills. With N cores the token therefore visits every actor exactly once → N
 * deliveries total. The five derived senders exercise the five emit surfaces:
 *   push<E> · send<E> · to(id).push<E> · getPipe(id).push<E> · getPipe(id).allocated_push<E>(n).
 *
 * Strengthened over the original (which asserted only `!hasError()` — a no-op `onInit()` would have
 * passed vacuously):
 *   - a global delivery counter must equal EXACTLY `max_core` after `join()` (every ring hop landed,
 *     no more, no less) — the load-bearing anti-vacuity guard;
 *   - the `allocated_push` implementation additionally asserts a receiver actually OBSERVED
 *     `has_extra_data == true` (the tail-payload travelled and was flagged), while the four
 *     non-allocated implementations assert it was never set — proving the flag is implementation-true,
 *     not incidental;
 *   - the suite `GTEST_SKIP`s on a single-core runner (a ring needs >= 2 cores) instead of asserting
 *     hardware with `EXPECT_GT`.
 *
 * No wall-clock oracle: each actor self-kills the instant it has forwarded the token; the ctest
 * TIMEOUT is the only backstop. Globals are reset per run so each parametrised case is independent.
 */

#include <atomic>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

// Self-validating payload event, shared with the messaging delivery tests.
#include "../../shared/ChecksumEvent.h"
using qb::test::copyAllocatedPayload;
using qb::test::TestEvent;

namespace {

// Per-run oracles (reset in each fixture SetUp).
std::atomic<std::uint32_t> g_delivered{0};       // total TestEvent receipts across the ring
std::atomic<bool>          g_checksum_ok{true};   // every received payload self-validated
std::atomic<bool>          g_extra_data_seen{false}; // some receiver saw has_extra_data == true

void
reset_oracles() {
    g_delivered.store(0, std::memory_order_relaxed);
    g_checksum_ok.store(true, std::memory_order_relaxed);
    g_extra_data_seen.store(false, std::memory_order_relaxed);
}

[[nodiscard]] std::uint32_t
ring_core_count() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0u ? 1u : static_cast<std::uint32_t>(hw);
}

struct MyTag {};

template <typename Derived>
class BaseActorSender : public qb::ServiceActor<MyTag> {
protected:
    qb::ActorId _to;

public:
    BaseActorSender()
        : _to(getServiceId<MyTag>((getIndex() + 1) % ring_core_count())) {
        registerEvent<TestEvent>(*this);
    }

    // The initial send must run once the object is fully constructed as Derived. Doing it in the
    // constructor would downcast *this to Derived before the Derived subobject exists (UB, flagged
    // by UBSan's vptr check). onInit() runs post-construction, when *this really is a Derived.
    qb::io::async::task<bool>
    onInit() override {
        if (!getIndex())
            static_cast<Derived &>(*this).doSend();
        co_return true;
    }

    void
    on(TestEvent const &event) {
        if (!event.checkSum())
            g_checksum_ok.store(false, std::memory_order_relaxed);
        if (event.has_extra_data)
            g_extra_data_seen.store(true, std::memory_order_relaxed);
        g_delivered.fetch_add(1, std::memory_order_relaxed);
        if (getIndex() != 0)
            static_cast<Derived &>(*this).doSend(); // forward the token to the next core
        kill();
    }
};

struct BasicPushActor : public BaseActorSender<BasicPushActor> {
    void
    doSend() {
        push<TestEvent>(_to);
    }
};

struct BasicSendActor : public BaseActorSender<BasicSendActor> {
    void
    doSend() {
        send<TestEvent>(_to);
    }
};

struct EventBuilderPushActor : public BaseActorSender<EventBuilderPushActor> {
    void
    doSend() {
        to(_to).push<TestEvent>();
    }
};

struct PipePushActor : public BaseActorSender<PipePushActor> {
    void
    doSend() {
        getPipe(_to).push<TestEvent>();
    }
};

struct AllocatedPipePushActor : public BaseActorSender<AllocatedPipePushActor> {
    void
    doSend() {
        auto &e          = getPipe(_to).allocated_push<TestEvent>(32);
        e.has_extra_data = true;
        copyAllocatedPayload(e);
    }
};

// Trait: does this implementation use the allocated-tail push (and thus set has_extra_data)?
template <typename T>
inline constexpr bool uses_extra_data = false;
template <>
inline constexpr bool uses_extra_data<AllocatedPipePushActor> = true;

template <typename ActorSender>
class ActorEventMulti : public testing::Test {
protected:
    const std::uint32_t max_core = ring_core_count();
    qb::Main            main;

    void
    SetUp() final {
        reset_oracles();
        for (auto i = 0u; i < max_core; ++i)
            main.addActor<ActorSender>(i);
    }
};

using Implementations = testing::Types<BasicPushActor, BasicSendActor, EventBuilderPushActor,
                                       PipePushActor, AllocatedPipePushActor>;

TYPED_TEST_SUITE(ActorEventMulti, Implementations);

TYPED_TEST(ActorEventMulti, SendEvents) {
    if (this->max_core < 2u)
        GTEST_SKIP() << "requires-multicore: a cross-core service ring needs >= 2 cores";

    this->main.start();
    this->main.join();

    EXPECT_FALSE(this->main.hasError());
    // Anti-vacuity oracle: the token visited every actor in the ring exactly once.
    EXPECT_EQ(g_delivered.load(), this->max_core)
        << "every one of the " << this->max_core << " ring actors must receive exactly one TestEvent";
    EXPECT_TRUE(g_checksum_ok.load()) << "every delivered payload must self-validate (exact bytes)";

    // has_extra_data is implementation-true: set iff this impl uses allocated_push.
    if constexpr (uses_extra_data<TypeParam>)
        EXPECT_TRUE(g_extra_data_seen.load())
            << "allocated_push must deliver an event flagged has_extra_data == true";
    else
        EXPECT_FALSE(g_extra_data_seen.load())
            << "a non-allocated send must never set has_extra_data";
}

} // namespace
