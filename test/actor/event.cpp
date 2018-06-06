#include "assert.h"
#include "cube.h"

struct MyEvent : public cube::Event {
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
    cube::ActorStatus status = cube::ActorStatus::Alive;
public:
    ActorTest() = default;

    cube::ActorStatus init() {
        this->template registerEvent<MyEvent>(*this);
        // Send event to myself
        this->template push<MyEvent>(this->id());
        return status;
    }

    cube::ActorStatus main() {
        return status;
    }

    // MyEvent call back
    void onEvent(MyEvent const &event) {
        this->template unRegisterEvent<MyEvent>(*this);
        // Send event to myself again but it will fail
        this->template push<MyEvent>(this->id());
    }

    // Overload when received removed event
    void onEvent (cube::Event const &event) {
        cube::Actor<Handler>::onEvent(event);
        // Kill me on next loop
        status = cube::ActorStatus::Dead;
    }
};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "log", "test-event.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<100>("Test un/register event", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1>> main;
        for (int i = 0; i < 1000; ++i) {
            main.addActor<0, ActorTest>();
            main.addActor<1, ActorTest>();
        }
        main.start();
        main.join();
        return 0;
    });
    return 0;
}