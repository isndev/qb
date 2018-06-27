#include "assert.h"
#include "cube.h"

struct CreateActorEvent : public cube::BestCoreEvent {
};

template <typename Handler>
class DummyActor : public cube::Actor<Handler>
        , public Handler::ICallback
{
    int _counter = 5;
public:
    DummyActor() = default;

    bool onInit() override final {
        this->registerCallback(*this);
        return true;
    }

    void onCallback() override final {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!--_counter) {
            this->kill();
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

    void on(CreateActorEvent const &event) {
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
        auto &e = this->template push<MyIntervalEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id(), cube::Timespan::seconds(1));
        e.repeat = 10;
        //LOG_INFO << "INIT ACTOR TEST";
        return true;
    }

    void on(MyTimedKillEvent const &)
    {
        this->template push<cube::KillEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<cube::service::IntervalActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<cube::service::ManagerActor<Handler>, 0>::id());
        this->template push<cube::KillEvent>(cube::Tag<MyAgent<Handler>, 2>::id());
        this->template push<cube::KillEvent>(cube::Tag<MyAgent<Handler>, 3>::id());
        this->kill();
        LOG_INFO << "DEAD ALL ACTOR TEST";
    }

    void on(MyIntervalEvent &event) {
        if (event.repeat <= 1) {
			event.cancel<MyIntervalEvent>(*this);
            this->template push<MyTimedKillEvent>(cube::Tag<cube::service::TimerActor<Handler>, 0>::id(), cube::Timespan::seconds(1));
        } else {
            this->template send<CreateActorEvent>(cube::Tag<cube::service::ManagerActor<Handler>, 0>::id());
        }
    }

};

using namespace cube;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-manager.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<1>("Test scheduled event", []() {
        Engine<PhysicalCore<0>, CoreLinker<TimedCore<2>, TimedCore<3>>> main;

        main.addActor<0, cube::service::TimerActor>();
        main.addActor<0, cube::service::IntervalActor>();
		main.addActor<2, MyAgent>();
		main.addActor<3, MyAgent>();
		main.addActor<0, cube::service::ManagerActor>();
        main.addActor<0, ActorTest>();

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
