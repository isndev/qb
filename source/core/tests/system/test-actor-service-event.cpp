/**
 * @file qb/core/tests/system/test-actor-service-event.cpp
 * @brief Unit tests for service actor event handling
 * 
 * This file contains tests for event handling between service actors in the
 * QB Actor Framework. It verifies that service actors can properly send, receive,
 * and validate events using various communication mechanisms.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

struct TestEvent : public qb::Event {
    uint8_t _data[32];
    uint32_t _sum;
    bool has_extra_data = false;

    TestEvent()
        : _sum(0) {
        std::random_device rand_dev;
        std::mt19937 generator(rand_dev());

        std::uniform_int_distribution<int> random_number(0, 255);
        std::generate(std::begin(_data), std::end(_data), [&]() {
            auto number = static_cast<uint8_t>(random_number(generator));
            _sum += number;
            return number;
        });
    }

    [[nodiscard]] bool
    checkSum() const {
        auto ret = true;
        if (has_extra_data) {
            ret = !memcmp(_data,
                          reinterpret_cast<const uint8_t *>(this) + sizeof(TestEvent),
                          sizeof(_data));
        }

        return std::accumulate(std::begin(_data), std::end(_data), 0u) == _sum && ret;
    }
};

struct MyTag {};

template <typename Derived>
class BaseActorSender : public qb::ServiceActor<MyTag> {
protected:
    qb::ActorId _to;

public:
    BaseActorSender()
        : _to(getServiceId<MyTag>((getIndex() + 1) %
                                  std::thread::hardware_concurrency())) {
        registerEvent<TestEvent>(*this);
        if (!getIndex())
            static_cast<Derived &>(*this).doSend();
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
        auto &e = getPipe(_to).allocated_push<TestEvent>(32);
        e.has_extra_data = true;
        memcpy(reinterpret_cast<uint8_t *>(&e) + sizeof(TestEvent), e._data,
               sizeof(e._data));
    }
};

template <typename ActorSender>
class ActorEventMulti : public testing::Test {
protected:
    const uint32_t max_core;
    qb::Main main;
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

typedef testing::Types<BasicPushActor, BasicSendActor, EventBuilderPushActor,
                       PipePushActor, AllocatedPipePushActor>
    Implementations;

TYPED_TEST_SUITE(ActorEventMulti, Implementations);

TYPED_TEST(ActorEventMulti, SendEvents) {
    EXPECT_GT(this->max_core, 1u);
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}