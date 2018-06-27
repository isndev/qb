#include "assert.h"
#include "cube.h"

struct MyEvent : public cube::Event {
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyEvent>(*this);
        // Send event to myself
        this->template push<MyEvent>(this->id());
        return true;
    }

    // MyEvent call back
    void on(MyEvent const &event) {
        this->template unregisterEvent<MyEvent>(*this);
        // Send event to myself again but it will fail
        this->template push<MyEvent>(this->id());
    }

    // Overload when received removed event
    void on (cube::Event const &event) {
        cube::Actor<Handler>::on(event);
        // Kill me on next loop
        this->kill();
    }
};

using namespace cube;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-event.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<100>("Test un/register event", []() {
        Engine<PhysicalCore<0>, PhysicalCore<1> > main;

        for (int i = 0; i < 2; ++i) {
            main.addActor<0, ActorTest>();
            main.addActor<1, ActorTest>();
        }
        main.start();
        main.join();
        return 0;
    });
    return 0;
}