#include "assert.h"
#include "service/scheduler/actor.h"
#include "cube.h"

using namespace cube::service::scheduler;

struct MyTimedEvent : public event::Timer {
    MyTimedEvent(cube::Timespan const &ts)
            : Timer(ts) {}
};

struct MyIntervalEvent : public event::Timeout {
    uint64_t i[32];
    MyIntervalEvent(cube::Timespan const &ts)
            : Timeout(ts) {
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
        this->template push<MyTimedEvent>(getServiceId<TimerTag>(0), cube::Timespan::seconds(1));
        auto &e = this->template push<MyIntervalEvent>(getServiceId<TimeoutTag>(0), cube::Timespan::seconds(1));
        e.repeat = 3;
        return true;
    }

    // MyEvent call back
    void on(MyTimedEvent const &event) {
        this->template push<cube::KillEvent>(getServiceId<TimerTag>(0));
    }

    void on(MyIntervalEvent &event) {
        event.cancel<MyIntervalEvent>(*this);
        this->template push<cube::KillEvent>(getServiceId<TimeoutTag>(0));
        this->kill();
    }

};

using namespace cube;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::DEBUG);

    test<1>("Test scheduled event", []() {
        Cube main({0, 1});

        main.addActor<ActorTimer>(0);
        main.addActor<ActorTimeout>(0);
        main.addActor<ActorTest>(1);

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
