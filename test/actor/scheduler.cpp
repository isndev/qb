#include "assert.h"
#include "cube.h"

struct MyTimedEvent : public cube::TimedEvent {
    MyTimedEvent(cube::Timespan const &ts)
            : TimedEvent(ts) {}
};

struct MyIntervalEvent : public cube::IntervalEvent {
    uint64_t i[32];
    MyIntervalEvent(cube::Timespan const &ts)
            : IntervalEvent(ts) {
        i[31] = 666;
    }

};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyTimedEvent>(*this);
        this->template registerEvent<MyIntervalEvent>(*this);
        // Send event to myself
        this->template push<MyTimedEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id(), cube::Timespan::seconds(1));
        auto &e = this->template push<MyIntervalEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id(), cube::Timespan::seconds(1));
        e.repeat = 3;
        return true;
    }

    // MyEvent call back
    void onEvent(MyTimedEvent const &event) {
        this->template push<cube::KillEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id());
    }

    void onEvent(MyIntervalEvent &event) {
        event.cancel<MyIntervalEvent>(*this);
        this->template push<cube::KillEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id());
        this->kill();
    }

};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::DEBUG);

    test<1>("Test scheduled event", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1> > main;

        main.addActor<0, cube::service::TimerActor>();
        main.addActor<0, cube::service::IntervalActor>();
        main.addActor<1, ActorTest>();

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
