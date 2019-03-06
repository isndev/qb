#include "assert.h"
#include "engine/service/scheduler/actor.h"
#include "cube.h"

using namespace qb::service::scheduler;

struct MyTimedEvent : public event::TimedEvent {
    MyTimedEvent(qb::Timespan const &ts)
            : TimedEvent(ts) {}
};

struct MyIntervalEvent : public event::TimedEvent {
    uint64_t i[32];
    MyIntervalEvent(qb::Timespan const &ts)
            : TimedEvent(ts) {
        i[31] = 666;
    }

};

class ActorTest : public qb::Actor {
public:
    ActorTest() = default;

    bool onInit() override final {
        registerEvent<MyTimedEvent>(*this);
        registerEvent<MyIntervalEvent>(*this);
        // Send event to myself
        auto &e = push<MyIntervalEvent>(getServiceId<Tag>(0), qb::Timespan::seconds(1));
        e.repeat = 3;
        return true;
    }

    // MyEvent call back
    void on(MyTimedEvent const &event) {
        push<qb::KillEvent>(getServiceId<Tag>(0));
        this->kill();
    }

    void on(MyIntervalEvent &event) {
        if (event.repeat == 2) {
            event.cancel<MyIntervalEvent>(*this);
            push<MyTimedEvent>(getServiceId<Tag>(0), qb::Timespan::seconds(3));
        }
    }

};

using namespace qb;
int main(int argc, char *argv[]) {
    qb::io::log::init("./", argv[0]);
    qb::io::log::setLevel(qb::io::log::Level::WARN);

    test<1>("Test scheduled event", [](auto &timer) {
        Cube main({0, 1});

        main.addActor<qb::service::scheduler::Actor>(0);
        main.addActor<ActorTest>(1);

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
