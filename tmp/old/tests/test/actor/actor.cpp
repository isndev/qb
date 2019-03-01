#include "assert.h"
#include "cube.h"

struct MyTrait {
    using _1 = int;
    using _2 = double;
};

class ActorTest : public cube::Actor
                , public cube::ICallback
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
class ActorTraitTest : public cube::Actor
                     , public cube::ICallback
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

using namespace cube;
int main(int argc, char *argv[]) {
    cube::io::log::init("./", argv[0]);
    cube::io::log::setLevel(cube::io::log::Level::WARN);

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