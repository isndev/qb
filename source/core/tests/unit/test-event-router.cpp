/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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
#include <qb/system/event/router.h>

struct ActorId  {
    uint32_t _id;

    ActorId() = default;
    ActorId(uint32_t id) : _id(id) {}

    operator uint32_t () const { return _id; }

    bool is_valid() const { return _id != 0; }
    bool is_broadcast() const { return _id == std::numeric_limits<uint32_t>::max(); }
    bool operator==(ActorId const &rhs) const noexcept { return _id == rhs._id; }

    static const ActorId broadcastId;
};

const ActorId ActorId::broadcastId = {std::numeric_limits<uint32_t>::max()};

namespace std {
    template<> struct hash<::ActorId> {
        std::size_t operator()(ActorId const &val) const noexcept {
            return static_cast<uint32_t>(val);
        }
    };
}

template<typename T>
struct type {
    constexpr static void id() {}
};

struct RawEvent {
    RawEvent() = default;
    RawEvent(RawEvent const &) = delete;
    using id_type = const char *;
    using id_handler_type = ActorId;

//    template<typename T>
//    constexpr static id_type type_to_id() { return static_cast<id_type>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    template<typename T>
    constexpr static id_type type_to_id() { return typeid(T).name(); }

    id_type id = "RawEvent";
    id_handler_type dest;
    id_handler_type source;
    bool alive = true;

    id_type getID() const noexcept { return id; }
    bool is_alive() const noexcept { return alive; }
    id_handler_type getDestination() const noexcept { return dest; }
};

struct TestEvent : public RawEvent {
    static std::size_t _count;

    TestEvent() {
        id = RawEvent::type_to_id<TestEvent>();
    }
};

struct TestConstEvent : public RawEvent {
    static std::size_t _count;

    TestConstEvent() {
        id = RawEvent::type_to_id<TestConstEvent>();
    }
};

struct TestDestroyEvent : public RawEvent {
    static std::size_t _count;

    TestDestroyEvent() {
        alive = false;
        id = RawEvent::type_to_id<TestDestroyEvent>();
    }

    ~TestDestroyEvent() {
        ++_count;
    }
};


std::size_t TestEvent::_count = 0;
std::size_t TestConstEvent::_count = 0;
std::size_t TestDestroyEvent::_count = 0;

void reset_all_event_counts() {
    TestEvent::_count = 0;
    TestConstEvent::_count = 0;
    TestDestroyEvent::_count = 0;
}

struct FakeActor {
    ActorId _id;

    FakeActor(uint32_t id) : _id(id) {}
    FakeActor(FakeActor const &) = delete;

    ActorId id() const {
        return _id;
    }

    bool is_alive() const noexcept { return true; }
    void on(TestEvent &event) {
        ++event._count;
    }

    void on(TestConstEvent const &event) const {
        ++event._count;
    }

    void on(TestDestroyEvent const &event) const {}
};

template <typename _Event, bool _CleanEvent = true>
void Test_SESH(std::size_t expected_count) {
    _Event event;
    FakeActor actor(1);

    _Event::_count = 0;
    for (auto i = 0; i < 1024; ++i) {
        qb::router::sesh<_Event, FakeActor>(actor).template route<_CleanEvent>(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, SESH) {
    Test_SESH<TestEvent>(1024);
    Test_SESH<const TestConstEvent>(1024);
    Test_SESH<TestDestroyEvent>(1024);
    Test_SESH<TestDestroyEvent, false>(0);
}

template <typename _Event, typename _Handler = void, bool _CleanEvent = true>
void Test_SEMH(std::size_t expected_count) {
    std::remove_const_t<_Event> event;
    FakeActor actor1(1);
    FakeActor actor2(2);
    FakeActor actor3(3);

    qb::router::semh<_Event, _Handler> semhRouter;

    semhRouter.subscribe(actor1);
    semhRouter.subscribe(actor2);
    semhRouter.subscribe(actor3);
    semhRouter.unsubscribe(actor1.id());

    _Event::_count = 0;
    for (auto i = 0; i < 1024; ++i) {
        for (auto j = 1; j < 4; ++j) {
            event.dest = j;
            semhRouter.template route<_CleanEvent>(event);
        }
        event.dest = ActorId::broadcastId;
        semhRouter.template route<_CleanEvent>(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, SEMH) {
    Test_SEMH<TestEvent>(4096);
    Test_SEMH<const TestConstEvent>(4096);
    Test_SEMH<TestDestroyEvent>(4096);
    Test_SEMH<TestDestroyEvent, void, false>(0);
    Test_SEMH<TestEvent, FakeActor>(4096);
    Test_SEMH<const TestConstEvent, FakeActor>(4096);
    Test_SEMH<TestDestroyEvent, FakeActor>(4096);
    Test_SEMH<TestDestroyEvent, FakeActor, false>(0);
}

template <typename _Event, bool _CleanEvent = true>
void Test_MESH(std::size_t expected_count) {
    std::remove_const_t<_Event> event;
    FakeActor actor1(1);

    qb::router::mesh<RawEvent, FakeActor, _CleanEvent> meshRouter(actor1);

    meshRouter.template subscribe<_Event>();

    _Event::_count = 0;
    for (auto i = 0; i < 1024; ++i) {
        meshRouter.route(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, MESH) {
    Test_MESH<TestEvent>(1024);
    Test_MESH<const TestConstEvent>(1024);
    Test_MESH<TestDestroyEvent>(1024);
    Test_MESH<TestDestroyEvent, false>(0);
}


template <typename _Event, bool _CleanEvent = true, typename _Handler = void>
void Test_MEMH(std::size_t expected_count) {
    std::remove_const_t<_Event> event;
    FakeActor actor1(1);
    FakeActor actor2(2);
    FakeActor actor3(3);
    FakeActor actor4(4);
    FakeActor actor5(5);
    qb::router::memh<RawEvent, _CleanEvent, _Handler> memhRouter;

    memhRouter.template subscribe<_Event>(actor1);
    memhRouter.template subscribe<_Event>(actor2);
    memhRouter.template subscribe<_Event>(actor3);
    memhRouter.template subscribe<_Event>(actor4);
    memhRouter.template subscribe<_Event>(actor5);
    memhRouter.unsubscribe(actor1.id());
    memhRouter.unsubscribe(actor2);
    memhRouter.template unsubscribe<_Event>(actor3);

    _Event::_count = 0;
    for (auto i = 0; i < 1024; ++i) {
        for (auto j = 1; j < 6; ++j) {
            event.dest = j;
            memhRouter.route(event);
        }
        event.dest = ActorId::broadcastId;
        memhRouter.route(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}


TEST(EventRouting, MEMH) {
    Test_MEMH<TestEvent>(4096);
    Test_MEMH<const TestConstEvent>(4096);
    Test_MEMH<TestDestroyEvent>(6144);
    Test_MEMH<TestDestroyEvent, false>(0);
    Test_MEMH<TestEvent, true, FakeActor>(4096);
    Test_MEMH<const TestConstEvent, true, FakeActor>(4096);
    Test_MEMH<TestDestroyEvent, true, FakeActor>(6144);
    Test_MEMH<TestDestroyEvent, false, FakeActor>(0);
}