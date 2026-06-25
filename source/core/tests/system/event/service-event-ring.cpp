/**
 * @file qb/core/tests/system/test-actor-service-event.cpp
 * @brief Unit tests for service actor event handling
 *
 * This file contains tests for event handling between service actors in the
 * QB Actor Framework. It verifies that service actors can properly send, receive,
 * and validate events using various communication mechanisms.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Core
 */

#include <gtest/gtest.h>
#include <numeric>
#include <qb/actor.h>
#include <qb/main.h>
#include <random>

// Self-validating payload event, shared with the messaging delivery tests.
#include "../../shared/ChecksumEvent.h"
using qb::test::copyAllocatedPayload;
using qb::test::TestEvent;

struct MyTag {};

template <typename Derived>
class BaseActorSender : public qb::ServiceActor<MyTag> {
protected:
    qb::ActorId _to;

public:
    BaseActorSender()
        : _to(getServiceId<MyTag>((getIndex() + 1) % std::thread::hardware_concurrency())) {
        registerEvent<TestEvent>(*this);
    }

    // The initial send must run once the object is fully constructed as Derived.
    // Doing it in the constructor downcasts *this to Derived before the Derived
    // subobject exists (undefined behaviour, flagged by UBSan's vptr check).
    // onInit() runs post-construction, when *this really is a Derived.
    qb::io::async::task<bool>
    onInit() override {
        if (!getIndex())
            static_cast<Derived &>(*this).doSend();
        co_return true;
    }

    void
    on(TestEvent const &event) {
        EXPECT_TRUE(event.checkSum());
        if (getIndex() != 0)
            static_cast<Derived &>(*this).doSend();
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

template <typename ActorSender>
class ActorEventMulti : public testing::Test {
protected:
    const uint32_t max_core;
    qb::Main       main;
    ActorEventMulti()
        : max_core(std::thread::hardware_concurrency()) {}

    void
    SetUp() final {
        for (auto i = 0u; i < max_core; ++i) {
            main.addActor<ActorSender>(i);
        }
    }
    void
    TearDown() final {}
};

typedef testing::Types<BasicPushActor, BasicSendActor, EventBuilderPushActor, PipePushActor, AllocatedPipePushActor> Implementations;

TYPED_TEST_SUITE(ActorEventMulti, Implementations);

TYPED_TEST(ActorEventMulti, SendEvents) {
    EXPECT_GT(this->max_core, 1u);
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}
