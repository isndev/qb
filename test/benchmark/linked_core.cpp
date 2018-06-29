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

template<typename Handler, typename EventTrait>
class ActorPong : public cube::Actor<Handler> {
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
        if (event.x >= 3000)
            this->kill();
        if (event.x <= 3000) {
            ++event.x;
            this->reply(event);
        }


    }

};

using namespace cube;
template<template<typename E, typename T> typename _ActorTest, typename _Event, typename _SharedData = void>
struct TEST {
    static void pingpong(std::string const &name) {
        test<100>("PingPong Linked Core0/1 (" + name + ")", []() {
            Engine<
                    CoreLinker<PhysicalCore<0>, PhysicalCore<1>>
            > main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest, _Event>(main.template addActor<0, _ActorTest, _Event>());
            }

            main.start();
            main.join();
            return 0;
        });

        test<100>("PingPong Core0/1 & Core2/3 (" + name + ")", []() {
            Engine<
                    CoreLinker<PhysicalCore<0>, PhysicalCore<1>>,
                    CoreLinker<PhysicalCore<2>, PhysicalCore<3>>
            > main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest, _Event>(main.template addActor<0, _ActorTest, _Event>());
                main.template addActor<3, _ActorTest, _Event>(main.template addActor<2, _ActorTest, _Event>());
            }

            main.start();
            main.join();
            return 0;
        });

        test<100>("PingPong Not Linked Core0/1 (" + name + ")", []() {
            Engine<PhysicalCore<0>, PhysicalCore<1>> main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest, _Event>(main.template addActor<0, _ActorTest, _Event>());
            }

            main.start();
            main.join();
            return 0;
        });

        test<100>("PingPong Not Linked Core0/3 (" + name + ")", []() {
            Engine<PhysicalCore<0>, PhysicalCore<3>> main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<3, _ActorTest, _Event>(main.template addActor<0, _ActorTest, _Event>());
            }

            main.start();
            main.join();
            return 0;
        });

        test<100>("PingPong Multi Not Linked Core0/1 & Core2/3 (" + name + ")", []() {
            Engine<
                    CoreLinker<PhysicalCore<0>, PhysicalCore<1>>,
                    CoreLinker<PhysicalCore<2>, PhysicalCore<3>>
            > main;

            for (int i = 0; i < 100; ++i) {
                main.template addActor<1, _ActorTest, _Event>(main.template addActor<3, _ActorTest, _Event>());
                main.template addActor<0, _ActorTest, _Event>(main.template addActor<2, _ActorTest, _Event>());
            }

            main.start();
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