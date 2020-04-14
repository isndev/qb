/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
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

struct RemovedEvent : public qb::Event {};

class TestActorReceiver : public qb::Actor {
    const uint32_t _max_events;
    uint32_t _count;

public:
    explicit TestActorReceiver(uint32_t const max_events)
        : _max_events(max_events)
        , _count(0) {
        registerEvent<TestEvent>(*this);
        registerEvent<RemovedEvent>(*this);
        unregisterEvent<RemovedEvent>(*this);
    }

    ~TestActorReceiver() final {
        EXPECT_EQ(_count, _max_events);
    }

    void
    on(TestEvent const &event) {
        EXPECT_TRUE(event.checkSum());
        if (++_count >= _max_events)
            kill();
    }

    void
    on(RemovedEvent const &) {}
};

class BaseSender {
public:
    const uint32_t _max_events;
    const qb::ActorId _to;
    uint32_t _count;

public:
    explicit BaseSender(uint32_t const max_events, qb::ActorId const to)
        : _max_events(max_events)
        , _to(to)
        , _count(0) {}
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
    BaseActorSender(uint32_t const max_events, qb::ActorId const to)
        : BaseSender(max_events, to) {}

    bool
    onInit() final {
        registerCallback(*this);
        return true;
    }

    void
    onCallback() final {
        static_cast<Derived &>(*this).doSend();
        if (++_count >= _max_events)
            kill();
    }
};

struct BasicPushActor : public BaseActorSender<BasicPushActor> {
    BasicPushActor(uint32_t const max_events, qb::ActorId const to)
        : BaseActorSender(max_events, to) {}
    void
    doSend() {
        push<TestEvent>(_to);
    }
};

struct BasicSendActor : public BaseActorSender<BasicSendActor> {
    BasicSendActor(uint32_t const max_events, qb::ActorId const to)
        : BaseActorSender(max_events, to) {}
    void
    doSend() {
        send<TestEvent>(_to);
    }
};

struct EventBuilderPushActor : public BaseActorSender<EventBuilderPushActor> {
    EventBuilderPushActor(uint32_t const max_events, qb::ActorId const to)
        : BaseActorSender(max_events, to) {}
    void
    doSend() {
        to(_to).push<TestEvent>();
    }
};

struct PipePushActor : public BaseActorSender<PipePushActor> {
    PipePushActor(uint32_t const max_events, qb::ActorId const to)
        : BaseActorSender(max_events, to) {}
    void
    doSend() {
        getPipe(_to).push<TestEvent>();
    }
};

struct AllocatedPipePushActor : public BaseActorSender<AllocatedPipePushActor> {
    AllocatedPipePushActor(uint32_t const max_events, qb::ActorId const to)
        : BaseActorSender(max_events, to) {}
    void
    doSend() {
        auto &e = getPipe(_to).allocated_push<TestEvent>(32);
        e.has_extra_data = true;
        memcpy(reinterpret_cast<uint8_t *>(&e) + sizeof(TestEvent), e._data,
               sizeof(e._data));
    }
};

#ifdef NDEBUG
constexpr uint32_t MAX_ACTORS = 1024u;
constexpr uint32_t MAX_EVENTS = 1024u;
#else
constexpr uint32_t MAX_ACTORS = 8u;
constexpr uint32_t MAX_EVENTS = 8u;
#endif

template <typename ActorSender>
class ActorEventMono : public testing::Test {
protected:
    qb::Main main;
    ActorEventMono() = default;
    void
    SetUp() final {
        for (auto i = 0u; i < MAX_ACTORS; ++i) {
            main.addActor<ActorSender>(0, MAX_EVENTS,
                                       main.addActor<TestActorReceiver>(0, MAX_EVENTS));
        }
    }
    void
    TearDown() final {}
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
            for (auto j = 0u; j < MAX_ACTORS; ++j) {
                main.addActor<ActorSender>(
                    i, MAX_EVENTS,
                    main.addActor<TestActorReceiver>(((i + 1) % max_core), MAX_EVENTS));
            }
        }
    }
    void
    TearDown() final {}
};

template <typename ActorSender>
class ActorEventBroadcastMono : public testing::Test {
protected:
    qb::Main main;
    ActorEventBroadcastMono() = default;
    void
    SetUp() final {
        main.addActor<ActorSender>(0, MAX_EVENTS, qb::BroadcastId(0));
        for (auto i = 0u; i < MAX_ACTORS; ++i) {
            main.addActor<TestActorReceiver>(0, MAX_EVENTS);
        }
    }
    void
    TearDown() final {}
};

template <typename ActorSender>
class ActorEventBroadcastMulti : public testing::Test {
protected:
    const uint32_t max_core;
    qb::Main main;
    ActorEventBroadcastMulti()
        : max_core(std::thread::hardware_concurrency()) {}

    void
    SetUp() final {
        for (auto i = 0u; i < max_core; ++i) {
            main.addActor<ActorSender>(i, MAX_EVENTS,
                                       qb::BroadcastId((i + 1) % max_core));
            for (auto j = 0u; j < MAX_ACTORS; ++j) {
                main.addActor<TestActorReceiver>(((i + 1) % max_core), MAX_EVENTS);
            }
        }
    }
    void
    TearDown() final {}
};

typedef testing::Types<BasicPushActor, BasicSendActor, EventBuilderPushActor,
                       PipePushActor, AllocatedPipePushActor>
    Implementations;

TYPED_TEST_SUITE(ActorEventMono, Implementations);
TYPED_TEST_SUITE(ActorEventBroadcastMono, Implementations);
TYPED_TEST_SUITE(ActorEventMulti, Implementations);
TYPED_TEST_SUITE(ActorEventBroadcastMulti, Implementations);

TYPED_TEST(ActorEventMono, SendEvents) {
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}

TYPED_TEST(ActorEventBroadcastMono, SendEvents) {
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}

TYPED_TEST(ActorEventMulti, SendEvents) {
    EXPECT_GT(this->max_core, 1u);
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}

TYPED_TEST(ActorEventBroadcastMulti, SendEvents) {
    EXPECT_GT(this->max_core, 1u);
    this->main.start();
    this->main.join();
    EXPECT_FALSE(this->main.hasError());
}

struct EventForward : public TestEvent {};

class TestSendReply : public qb::Actor {
    const qb::ActorId _to;
    uint32_t counter = 0;

public:
    explicit TestSendReply(qb::ActorId const to)
        : _to(to) {}

    ~TestSendReply() final {
        EXPECT_EQ(counter, 2u);
    }

    bool
    onInit() final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);

        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);

        if (_to.index()) {
            push<TestEvent>(qb::BroadcastId(_to.index()));
            push<EventForward>(qb::BroadcastId(_to.index()));
        }
        push<TestEvent>(_to);
        push<EventForward>(_to);

        return true;
    }

    void
    on(TestEvent &event) {
        ++counter;
        EXPECT_TRUE(event.checkSum());
    }

    void
    on(EventForward &event) {
        ++counter;
        EXPECT_TRUE(event.checkSum());
        push<qb::KillEvent>(qb::BroadcastId(_to.index()));
        kill();
    }
};

class TestReceiveReply : public qb::Actor {
    uint32_t counter = 0;

public:
    TestReceiveReply() = default;

    ~TestReceiveReply() final {
        if (id().index())
            EXPECT_EQ(counter, 4u);
        else
            EXPECT_EQ(counter, 2u);
    }

    bool
    onInit() final {
        EXPECT_NE(static_cast<uint32_t>(id()), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);
        return true;
    }

    void
    on(TestEvent &event) {
        EXPECT_TRUE(event.checkSum());
        reply(event);
        ++counter;
    }

    void
    on(EventForward &event) {
        EXPECT_TRUE(event.checkSum());
        forward(event.getSource(), event);
        ++counter;
    }
};

TEST(ActorEventMono, PushReplyForward) {
    qb::Main main;

    main.addActor<TestSendReply>(0, main.addActor<TestReceiveReply>(0));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(ActorEventMulti, PushReplyForward) {
    qb::Main main;

    main.addActor<TestSendReply>(0, main.addActor<TestReceiveReply>(1));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}