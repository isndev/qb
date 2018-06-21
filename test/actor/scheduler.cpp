#include "assert.h"
#include "cube.h"

struct MyEvent : public cube::TimedEvent {
  MyEvent(cube::Timespan const &ts)
    : TimedEvent(ts) {}
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;
  
    bool onInit() override final {
        this->template registerEvent<MyEvent>(*this);
        // Send event to myself
        auto &e = this->template push<MyEvent>(cube::Tag<cube::SchedulerActor<Handler>, 0>::id(), cube::Timespan::seconds(1));
        return true;
    }

    // MyEvent call back
    void onEvent(MyEvent const &event) {
      this->template push<cube::KillEvent>(cube::Tag<cube::SchedulerActor<Handler>, 0>::id());
        this->kill();
    }

};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::DEBUG);

    test<3>("Test scheduled event", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1> > main;

        auto id_sched = main.addActor<0, cube::SchedulerActor>();
        main.addActor<1, ActorTest>();

        main.start();
        main.join();
        return 0;
    });
    return 0;
}
