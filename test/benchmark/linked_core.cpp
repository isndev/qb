#include "cube.h"
#include "assert.h"
#define NB_ACTORS 1000
#define NB_PINGPONG 1000

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

template<typename EventTrait>
class ActorPong : public cube::Actor {
    const cube::ActorId actor_to_send;

public:
    ActorPong(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    bool onInit() override final {
        this->template registerEvent<EventTrait>(*this);
        if (actor_to_send)
            this->template push<EventTrait>(actor_to_send, 0);
        return true;
    }

    void on(EventTrait &event) const {
        if (event.x >= NB_PINGPONG)
            this->kill();
        if (event.x <= NB_PINGPONG) {
            ++event.x;
            this->reply(event);
        }


    }

};

using namespace cube;
template<template<typename T> typename _ActorTest, typename _Event, typename _SharedData = void>
struct TEST {
    static void pingpong(std::string const &name) {
        test<100>("PingPong Core0/1 (" + name + ")", [](auto &timer) {
            Cube main({0, 1});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(1, main.template addActor<_ActorTest, _Event>(0));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("PingPong Core1/2 (" + name + ")", [](auto &timer) {
            Cube main({1, 2});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(2, main.template addActor<_ActorTest, _Event>(1));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("PingPong Core2/3 (" + name + ")", [](auto &timer) {
            Cube main({2, 3});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(3, main.template addActor<_ActorTest, _Event>(2));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("PingPong  Core0/3 (" + name + ")", [](auto &timer) {
            Cube main({0, 3});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(3, main.template addActor<_ActorTest, _Event>(0));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("PingPong Core0/1 & Core2/3 (" + name + ")", [](auto &timer) {
            Cube main({0, 1, 2, 3});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(1, main.template addActor<_ActorTest, _Event>(0));
                main.template addActor<_ActorTest, _Event>(3, main.template addActor<_ActorTest, _Event>(2));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("PingPong Core0/2 & Core1/3 (" + name + ")", [](auto &timer) {
            Cube main({0, 1, 2, 3});

            for (int i = 0; i < NB_ACTORS; ++i) {
                main.template addActor<_ActorTest, _Event>(2, main.template addActor<_ActorTest, _Event>(0));
                main.template addActor<_ActorTest, _Event>(3, main.template addActor<_ActorTest, _Event>(1));
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-linked_core.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

    // ping pong
    TEST<ActorPong, TinyEvent>::pingpong("TinyEvent");
    TEST<ActorPong, BigEvent>::pingpong("BigEvent");
    TEST<ActorPong, DynamicEvent>::pingpong("DynamicEvent");

    return 0;
}