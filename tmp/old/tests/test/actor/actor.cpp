#include "assert.h"
#include "cube.h"

struct MyTrait {
    using _1 = int;
    using _2 = double;
};

class ActorTest : public qb::Actor
                , public qb::ICallback
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

template <typename Trait>
class ActorTraitTest : public qb::Actor
                     , public qb::ICallback
{
public:
    typename Trait::_1 x;
    typename Trait::_2 y;

    ActorTraitTest() {}

    bool onInit() override final {
        static int construct_time = 0;
        // add actor linked to same core
        addRefActor<ActorTest>();
        // add me
        if (construct_time++ < 100)
            addRefActor<ActorTraitTest>();
        this->registerCallback(*this);
        return true;
    }

    void onCallback() override final {
        this->kill();
    }
};

using namespace qb;
int main(int argc, char *argv[]) {
    qb::io::log::init("./", argv[0]);
    qb::io::log::setLevel(qb::io::log::Level::WARN);

    test<100>("CreateActor", [](auto &timer) {
        Cube main({0, 1});

        main.addActor<ActorTest>(0);
        main.addActor<ActorTraitTest, MyTrait>(1);

        main.start();
        timer.reset();
        main.join();
        return 0;
    });
    return 0;
}