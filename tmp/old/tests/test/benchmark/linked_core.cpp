#include "cube.h"
#include "assert.h"
#define NB_ACTORS 1000
#define NB_PINGPONG 1000



using namespace qb;
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

int main(int argc, char *argv[]) {
    qb::io::log::init("./", argv[0]);
    qb::io::log::setLevel(qb::io::log::Level::WARN);

    // ping pong
    TEST<ActorPong, TinyEvent>::pingpong("TinyEvent");
    TEST<ActorPong, BigEvent>::pingpong("BigEvent");
    TEST<ActorPong, DynamicEvent>::pingpong("DynamicEvent");

    return 0;
}
