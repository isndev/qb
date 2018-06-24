#include "assert.h"
#include "cube.h"

struct MyTrait {
    using _1 = int;
    using _2 = double;
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler>
                , public Handler::ICallback
{
public:
    ActorTest() = default;

    bool onInit() override final {
        this->registerCallback(*this);
        return true;
    }

    void onCallback() override final {
        this->kill();
    }
};

template <typename Handler, typename Trait>
class ActorTraitTest : public cube::Actor<Handler>
                     , public Handler::ICallback
{
public:
    typename Trait::_1 x;
    typename Trait::_2 y;

    ActorTraitTest() {}

    bool onInit() override final {
        static int construct_time = 0;
        // add actor linked to same core
        this-> template addRefActor<ActorTest>();
        // add me
        if (construct_time++ < 100)
            this-> template addRefActor<ActorTraitTest>();
        this->registerCallback(*this);
        return true;
    }

    void onCallback() override final {
        this->kill();
    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-actor.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<100>("CreateActor", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1>> main;

        main.addActor<0, ActorTest>();
        main.addActor<1, ActorTraitTest, MyTrait>();

        main.start();
        main.join();
        return 0;
    });
    return 0;
}