#include "assert.h"
#include "cube.h"

struct MyEvent : public qb::Event {
};

class ActorTest : public qb::Actor {
public:
    ActorTest() = default;

    bool onInit() override final {
        registerEvent<MyEvent>(*this);
        // Send event to myself
        push<MyEvent>(this->id());
        return true;
    }

    // MyEvent call back
    void on(MyEvent const &event) {
        unregisterEvent<MyEvent>(*this);
        // Send event to myself again but it will fail
        push<MyEvent>(this->id());
    }

    // Overload when received removed event
    void on (qb::Event const &event) {
        qb::Actor::on(event);
        // Kill me on next loop
        this->kill();
    }
};

using namespace qb;
int main(int argc, char *argv[]) {
    qb::io::log::init("./", argv[0]);
    qb::io::log::setLevel(qb::io::log::Level::WARN);

    test<100>("Test un/register event", [](auto &timer) {
        Cube main({0,1});

        for (int i = 0; i < 2; ++i) {
            main.addActor<ActorTest>(0);
            main.addActor<ActorTest>(1);
        }
        main.start();
        timer.reset();
        main.join();
        return 0;
    });
    return 0;
}