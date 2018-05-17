#include "cube.h"
#include "assert.h"

struct TinyEvent : cube::Event {
    uint64_t x;
    TinyEvent(uint64_t x) : x(x) {}
};

struct BigEvent : cube::Event {
    uint64_t x;
    uint64_t padding[127];
    BigEvent(uint64_t x) : x(x) {}
};

struct DynamicEvent : cube::Event {
    uint64_t x;
    std::vector<int> vec;
    DynamicEvent(uint64_t x) : x(x), vec(512, 8) {}
};

template<typename Handler = void>
class ActorMock_Tiny : public cube::Actor<Handler> {
    const cube::ActorId actor_to_send;
    int alive = 0;
public:
    ActorMock_Tiny(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    int init() {
        this->template registerEvent<TinyEvent>(*this);
        if (actor_to_send)
            this->template push<TinyEvent>(actor_to_send, 0);
        return 0;
    }

    int main() {
        return alive;
    }

    void onEvent(TinyEvent const &event) {
        if (event.x >= 3000) alive = 1;
        auto &rep = this->template reply<TinyEvent>(event);
        ++rep.x;
    }

};

template<typename Handler = void>
class ActorMock_Big : public cube::Actor<Handler> {
    const cube::ActorId actor_to_send;
    int alive = 0;
public:
    ActorMock_Big(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    int init() {
        this->template registerEvent<BigEvent>(*this);
        if (actor_to_send)
            this->template push<BigEvent>(actor_to_send, 0);
        return 0;
    }

    int main() {
        return alive;
    }

    void onEvent(BigEvent const &event) {
        if (event.x >= 3000) alive = 1;
        auto &rep = this->template reply<BigEvent>(event);
        ++rep.x;
    }

};

template<typename Handler = void>
class ActorMock_Dynamic : public cube::Actor<Handler> {
    const cube::ActorId actor_to_send;
    int alive = 0;
public:
    ActorMock_Dynamic(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    int init() {
        this->template registerEvent<DynamicEvent>(*this);
        if (actor_to_send)
            this->template push<DynamicEvent>(actor_to_send, 0);
        return 0;
    }

    int main() {
        return alive;
    }

    void onEvent(DynamicEvent const &event) {
        if (event.x >= 3000) alive = 1;
        auto &rep = this->template reply<DynamicEvent>(event);
        ++rep.x;
    }
};


template<template<typename T> typename _ActorTest, typename _SharedData = void>
struct TEST {
    static void pingpong(std::string const &name) {
        test<100>("PingPong Core0/1 (" + name + ")", []() {
            cube::Main<
                    CoreLink<PhysicalCore<0>, PhysicalCore<1>>
            > main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest>(main.template addActor<0, _ActorTest>());
            }

            main.start();
            main.join();
            return 0;
        });
        test<100>("PingPong Core0/1 & Core2/3 (" + name + ")", []() {
            cube::Main<
                    CoreLink<PhysicalCore<0>, PhysicalCore<1>>,
                    CoreLink<PhysicalCore<2>, PhysicalCore<3>>
            > main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest>(main.template addActor<0, _ActorTest>());
                main.template addActor<3, _ActorTest>(main.template addActor<2, _ActorTest>());
            }

            main.start();
            main.join();
            return 0;
        });
    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./", "linked_core.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

    // ping pong
    TEST<ActorMock_Tiny>::pingpong("TinyEvent");
    TEST<ActorMock_Big>::pingpong("BigEvent");
    TEST<ActorMock_Dynamic>::pingpong("DynamicEvent");

    return 0;
}