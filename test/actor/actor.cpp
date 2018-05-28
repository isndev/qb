#include "assert.h"
#include "cube.h"

struct MyTrait {
    using _1 = int;
    using _2 = double;
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    cube::ActorStatus main() {
        return cube::ActorStatus::Dead;
    }
};

template <typename Trait, typename Handler>
class ActorTraitTest : public cube::Actor<Handler> {
public:
    typename Trait::_1 x;
    typename Trait::_2 y;

    ActorTraitTest() = default;

    cube::ActorStatus init() {
        static int construct_time = 0;
        // add actor linked to same core
        this-> template addRefActor<ActorTest>();
        // add me
        if (construct_time++ < 100)
            this-> template addRefActor<ActorTraitTest>();

        return cube::ActorStatus::Alive;
    }

    cube::ActorStatus main() {
        return cube::ActorStatus::Dead;
    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./", "test-actor.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<100>("CreateActor", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1>> main;

        main.addActor<0, ActorTest>();
        main.addActor<1, ActorTraitTest, MyTrait>();

        main.start();
        main.join();
        return 0;
    });
}