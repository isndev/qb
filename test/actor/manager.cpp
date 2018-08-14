#include "assert.h"
#include "cube.h"

using scheduler = cube::service::scheduler::Tags<0>;
using manager = cube::service::manager::Tags<0>;

struct CreateActorEvent : public cube::service::manager::event::ToBestTimedCore {
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
class MyAgent : public cube::service::manager::ActorAgent<Handler> {
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

struct MyTimeoutEvent : public cube::service::scheduler::event::Timeout {
    MyTimeoutEvent(cube::Timespan const &ts)
            : Timeout(ts) {}
};

struct MyTimedEvent : public cube::service::scheduler::event::Timer {
    MyTimedEvent(cube::Timespan const &ts)
            : Timer(ts) {}
};


template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
public:
    ActorTest() = default;

    bool onInit() override final {
        this->template registerEvent<MyTimeoutEvent>(*this);
        this->template registerEvent<MyTimedEvent>(*this);
        // Send event to myself
        auto &e = this->template push<MyTimeoutEvent>(scheduler::id_timeout(), cube::Timespan::seconds(1));
        e.repeat = 10;
        //LOG_INFO << "INIT ACTOR TEST";
        return true;
    }

    void on(MyTimedEvent const &)
    {
        this->template push<cube::KillEvent>(scheduler::id_timer());
        this->template push<cube::KillEvent>(scheduler::id_timeout());
        this->template push<cube::KillEvent>(manager::id());
        this->template push<cube::KillEvent>({manager::uid_agent, 2});
        this->template push<cube::KillEvent>({manager::uid_agent, 3});
        this->kill();
        LOG_INFO << "DEAD ALL ACTOR TEST";
    }

    void on(MyTimeoutEvent &event) {
        if (event.repeat <= 1) {
            event.cancel<MyTimeoutEvent>(*this);
            this->template push<MyTimedEvent>(scheduler::id_timer(), cube::Timespan::seconds(1));
        } else {
            this->template send<CreateActorEvent>(manager::id());
        }
    }

};

using namespace cube;
using namespace cube::service;
int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./log/", "test-manager.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);

    test<1>("Test scheduled event", []() {
        Engine<PhysicalCore<0>, CoreLinker<TimedCore<2>, TimedCore<3>>> main;

        main.addActor<0, scheduler::ActorTimer>();
        main.addActor<0, scheduler::ActorTimeout>();
        main.addActor<2, MyAgent>();
        main.addActor<3, MyAgent>();
        main.addActor<0, manager::Actor>();
        main.addActor<0, ActorTest>();

        main.start();
        main.join();
        return 0;
    });


    return 0;
}
