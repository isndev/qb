#include "cube.h"
#include "assert.h"

struct SharedDataMock {
    std::vector<int> shared_vector;

    SharedDataMock() : shared_vector(){}
};

template<typename Handler = void>
class ActorMock_Shared : public cube::Actor<Handler> {
    uint32_t _counter;
public:
    ActorMock_Shared() : _counter(1) {}

    cube::ActorStatus main() {
        auto &data = this->sharedData();
        if (data.shared_vector.size() < 1000000) {
            data.shared_vector.push_back(_counter++);
            return cube::ActorStatus::Alive;
        }

        return cube::ActorStatus::Dead;
    }
};

template<template<typename T> typename _ActorTest, typename _SharedData, std::size_t ..._Index>
struct TEST {
    static void shared_data(std::string const &name) {
        test<100>("SharedData (" + name + ")", []() {
            cube::Main<PhysicalCore<_Index, _SharedData>...> main;

            for (int i = 0; i < 100; ++i) {
                (main.template addActor<_Index, _ActorTest>() && ...);
            }

            main.start();
            main.join();
            return 0;
        });

    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "log", "test-shared_data.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

    TEST<ActorMock_Shared, SharedDataMock, 0>::shared_data("Core0");
    TEST<ActorMock_Shared, SharedDataMock, 0, 1>::shared_data("Core0/1");
    TEST<ActorMock_Shared, SharedDataMock, 0, 3>::shared_data("Core0/3");
    TEST<ActorMock_Shared, SharedDataMock, 0, 1, 2, 3>::shared_data("Core0/1/2/3");

    return 0;
}