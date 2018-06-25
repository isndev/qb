#include "assert.h"
#include "cube.h"

struct CreateActorEvent : public cube::BestCoreEvent {
};

template <typename Handler>
class DummyActor : public cube::Actor<Handler>
        , public Handler::ICallback
{
    int _counter = 100000;
public:
    DummyActor() = default;

    bool onInit() override final {
        this->registerCallback(*this);
        return true;
    }

    void onCallback() override final {

        if (!--_counter) {
            this->kill();
          //LOG_INFO << "DEAD DUMMY ACTOR";
        }
    }
};


template <typename Handler>
class MyAgent : public cube::service::ManagerAgentActor<Handler> {
public:
    bool onInit() override final {
        this->template registerEvent<CreateActorEvent>(*this);
        return true;
    }

    void onEvent(CreateActorEvent const &event) {
        LOG_INFO << "AGENT CREATE ON CORE(" << this->id()._index << ")";
        this->template addRefActor<DummyActor>();
    }
};

struct MyIntervalEvent : public cube::IntervalEvent {
    MyIntervalEvent(cube::Timespan const &ts)
            : IntervalEvent(ts) {}
};

struct MyTimedKillEvent : public cube::TimedEvent {
    MyTimedKillEvent(cube::Timespan const &ts)
            : TimedEvent(ts) {}
};


template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyIntervalEvent>(*this);
        this->template registerEvent<MyTimedKillEvent>(*this);
        // Send event to myself
        auto &e = this->template push<MyIntervalEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id(), cube::Timespan::microseconds(100));
        e.repeat = 1000;
        //LOG_INFO << "INIT ACTOR TEST";
        return true;
    }

    void onEvent(MyTimedKillEvent const &)
    {
        this->template push<cube::KillEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<cube::service::ManagerActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<MyAgent<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<MyAgent<Handler>, 1>::id());
        this->kill();
        LOG_INFO << "DEAD ALL ACTOR TEST";
    }

    void onEvent(MyIntervalEvent const &event) {
        if (event.repeat <= 1) {
            this->template push<MyTimedKillEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id(), cube::Timespan::seconds(3));
        } else {
            this->template send<CreateActorEvent>(cube::Tag<cube::service::ManagerActor<Handler>, 0>::id());
            //LOG_INFO << "SEND TO AGENT";
        }
    }

};

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-scheduler.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<1>("Test scheduled event", []() {
        cube::Main<PhysicalCore<0>, PhysicalCore<1> > main;

        main.addActor<0, cube::service::TimerActor>();
        main.addActor<0, cube::service::IntervalActor>();
        main.addActor<0, MyAgent>();
        main.addActor<1, MyAgent>();
        main.addActor<0, cube::service::ManagerActor>();
        main.addActor<1, ActorTest>();

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
