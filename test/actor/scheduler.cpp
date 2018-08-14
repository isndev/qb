#include "assert.h"
#include "cube.h"

using namespace cube::service::scheduler;
using scheduler_tag = cube::service::scheduler::Tags<0>;

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

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyTimedEvent>(*this);
        this->template registerEvent<MyIntervalEvent>(*this);
        // Send event to myself
        this->template push<MyTimedEvent>(scheduler_tag::id_timer(), cube::Timespan::seconds(1));
        auto &e = this->template push<MyIntervalEvent>(scheduler_tag::id_timeout(), cube::Timespan::seconds(1));
        e.repeat = 3;
        return true;
    }

    // MyEvent call back
    void on(MyTimedEvent const &event) {
        this->template push<cube::KillEvent>(scheduler_tag::id_timer());
    }

    void on(MyIntervalEvent &event) {
        event.cancel<MyIntervalEvent>(*this);
        this->template push<cube::KillEvent>(scheduler_tag::id_timeout());
        this->kill();
    }

};

using namespace cube;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::DEBUG);

    test<1>("Test scheduled event", []() {
        Engine<PhysicalCore<0>, PhysicalCore<1> > main;

        main.addActor<0, ActorTimer>();
        main.addActor<0, ActorTimeout>();
        main.addActor<1, ActorTest>();

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
