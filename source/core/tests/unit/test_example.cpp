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
};

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

    id_type id = "";
    id_handler_type dest;
    id_handler_type source;

    id_type getID() const noexcept { return id; }
    bool is_alive() const noexcept { return true; }
    id_handler_type getDestination() const noexcept { return dest; }
};

struct TestEvent : public RawEvent {
    TestEvent() {
        id = RawEvent::type_to_id<TestEvent>();
    }
};

struct TestEvent2 : public RawEvent {
    TestEvent2() {
        id = RawEvent::type_to_id<TestEvent2>();
    }
};

struct FakeActor {
    ActorId _id;

    FakeActor(uint32_t id) : _id(id) {}
    FakeActor(FakeActor const &) = delete;

    ActorId id() const {
        return _id;
    }

    bool is_alive() const noexcept { return true; }
    void on(RawEvent const &event) {
        std::cout << "Actor(" << _id  << ") RawEvent id=" << event.id << std::endl;
    }

    void on(TestEvent const &event) {
        std::cout << "Actor(" << _id  << ") TestEvent id=" << event.id << std::endl;
    }

    void on(TestEvent2 const &event)  {
        std::cout << "Actor(" << _id  << ") TestEvent2 id=" << event.id << std::endl;
    }
};

TEST(Example, StartEngine) {
    RawEvent event;
    TestEvent tevent;
    TestEvent2 tevent2;
    FakeActor actor1(1);
    FakeActor actor2(2);
    {
        std::cout << " --- SESHRouter --- " << std::endl;
        qb::router::sesh<const RawEvent, FakeActor>(actor1).route(event);
        qb::router::sesh<RawEvent, FakeActor>(actor2).route(event);
    }
    {
        std::cout << " --- SEMHRouter --- " << std::endl;
        qb::router::semh<RawEvent> semhRouter;
        semhRouter.subscribe(actor1);
        semhRouter.subscribe(actor2);
        event.dest = 1;
        semhRouter.route(event);
        event.dest = 2;
        semhRouter.route(event);
        event.dest = std::numeric_limits<uint32_t>::max();
        semhRouter.route(event);
        semhRouter.unsubscribe(1);
        semhRouter.unsubscribe(2);
    }
    {
        std::cout << " --- SEMHRouter<FakeActor> --- " << std::endl;
        qb::router::semh<RawEvent, FakeActor> semhRouter;
        semhRouter.subscribe(actor1);
        semhRouter.subscribe(actor2);
        event.dest = 1;
        semhRouter.route(event);
        event.dest = 2;
        semhRouter.route(event);
        event.dest = std::numeric_limits<uint32_t>::max();
        semhRouter.route(event);
        semhRouter.unsubscribe(1);
        semhRouter.unsubscribe(2);
    }
    {
        std::cout << " --- MESHRouter --- " << std::endl;
        qb::router::mesh<RawEvent, FakeActor> meshRouter(actor1);
        meshRouter.subscribe<TestEvent>();
        meshRouter.subscribe<TestEvent2>();
        tevent.dest = 1;
        meshRouter.route(tevent);
        tevent2.dest = 1;
        meshRouter.route(tevent2);
        meshRouter.unsubscribe();
    }
    {
        std::cout << " --- MEMHRouter --- " << std::endl;
        qb::router::memh<RawEvent> memhRouter;
        memhRouter.subscribe<TestEvent>(actor1);
        memhRouter.subscribe<TestEvent>(actor2);
        tevent.dest = 1;
        memhRouter.route(tevent);
        tevent.dest = 2;
        memhRouter.route(tevent);
        tevent.dest = std::numeric_limits<uint32_t>::max();
        memhRouter.route(tevent);
        memhRouter.unsubscribe(actor1);
        memhRouter.unsubscribe<TestEvent>(actor2);
    }
    {
        std::cout << " --- MEMHRouter<FakeActor> --- " << std::endl;
        qb::router::memh<RawEvent, FakeActor> memhRouter;
        memhRouter.subscribe<TestEvent>(actor1);
        memhRouter.subscribe<TestEvent>(actor2);
        tevent.dest = 1;
        memhRouter.route(tevent);
        tevent.dest = 2;
        memhRouter.route(tevent);
        tevent.dest = std::numeric_limits<uint32_t>::max();
        memhRouter.route(tevent);
        memhRouter.unsubscribe(actor1.id());
        memhRouter.unsubscribe<TestEvent>(actor2);
    }

//  qb::Main main({0});
//
//  main.start();
//  main.join();
//  EXPECT_TRUE(main.hasError());
}