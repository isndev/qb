#include "assert.h"
#include "service/scheduler/actor.h"
#include "cube.h"

using namespace cube::service::scheduler;

struct MyTimedEvent : public event::TimedEvent {
    MyTimedEvent(cube::Timespan const &ts)
            : TimedEvent(ts) {}
};

struct MyIntervalEvent : public event::TimedEvent {
    uint64_t i[32];
    MyIntervalEvent(cube::Timespan const &ts)
            : TimedEvent(ts) {
        i[31] = 666;
    }

};

class ActorTest : public cube::Actor {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyTimedEvent>(*this);
        this->template registerEvent<MyIntervalEvent>(*this);
        // Send event to myself
        auto &e = this->template push<MyIntervalEvent>(getServiceId<Tag>(0), cube::Timespan::seconds(1));
        e.repeat = 3;
        return true;
    }

    // MyEvent call back
    void on(MyTimedEvent const &event) {
        this->template push<cube::KillEvent>(getServiceId<Tag>(0));
        this->kill();
    }

    void on(MyIntervalEvent &event) {
        if (event.repeat == 2) {
            event.cancel<MyIntervalEvent>(*this);
            this->template push<MyTimedEvent>(getServiceId<Tag>(0), cube::Timespan::seconds(3));
        }
    }

};

using namespace cube;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::WARN);

    test<1>("Test scheduled event", [](auto &timer) {
        Cube main({0, 1});

        main.addActor<cube::service::scheduler::Actor>(0);
        main.addActor<ActorTest>(1);

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
