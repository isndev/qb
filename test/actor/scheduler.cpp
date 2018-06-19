#include "assert.h"
#include "cube.h"

struct MyEvent : public cube::TimedEvent {
    uint64_t data;
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    cube::ActorId id_sched;
    ActorTest() = delete;
    ActorTest(cube::ActorId const &id_sched)
            : id_sched(id_sched) {}

    bool onInit() override final {
        this->template registerEvent<MyEvent>(*this);
        // Send event to myself
        auto &e = this->template push<MyEvent>(id_sched);
        e.duration = (cube::NanoTimestamp() + cube::Timespan::seconds(1)).nanoseconds();
        return true;
    }

    // MyEvent call back
    void onEvent(MyEvent const &event) {
        this->template push<cube::KillEvent>(id_sched);
        this->kill();
    }

};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::DEBUG);

    test<3>("Test un/register event", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1> > main;

        auto id_sched = main.addActor<0, cube::SchedulerActor, void>();
        main.addActor<1, ActorTest>(id_sched);

        main.start();
        main.join();
        return 0;
    });
    return 0;
}