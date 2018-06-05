#include "assert.h"
#include "cube.h"

struct ChainEvent : cube::Event
{
    cube::ActorId first;
    uint64_t creation_time;
    uint64_t loop = 0;
};

template <typename Handler>
class ActorTest : public cube::Actor<Handler> {
    const bool first;
    const cube::ActorId to_send;
    cube::ActorStatus status = cube::ActorStatus::Alive;
public:
    ActorTest(cube::ActorId const id = {}, bool first = false)
            : first(first)
            , to_send(id) {}

    cube::ActorStatus init() {
        this-> template registerEvent<ChainEvent>(*this);
        if (first) {
            auto &event = this-> template push<ChainEvent>(to_send);
            event.first = this->id();
            event.creation_time = cube::Timestamp::rdts();
        }
        return cube::ActorStatus::Alive;
    }

    void onEvent(ChainEvent const &event) {
        if (event.loop >= 10000) {
            status = cube::ActorStatus::Dead;
            if (!to_send)
                LOG_INFO << "Event Time To Arrive " << cube::Timestamp::rdts() - event.creation_time << "ns";
        }
        auto &fwd = this-> forward(to_send ? to_send : event.first, event);
        if (first)
            fwd.creation_time = cube::Timestamp::rdts();
        if (!to_send) {
            ++fwd.loop;
        }
    }

    cube::ActorStatus main() {
        return status;
    }
};


void test_chain(int nb_actor) {
    test("Test ChainEvent " + std::to_string(nb_actor) + " Actor(s) per Core 1000 chain loop\n",
    [nb_actor]() {
        test<100>("ChainEvent 2 Unlinked Core", [nb_actor]() {
            cube::Main<PhysicalCore<0>, PhysicalCore<1>> main;
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<0, ActorTest>(main.addActor<1, ActorTest>(), true);
            }
            main.start();
            main.join();
            return 0;
        });

        test<100>("ChainEvent 2 Linked Core", [nb_actor]() {
            cube::Main<CoreLink<PhysicalCore<0>, PhysicalCore<1>>> main;
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<0, ActorTest>(main.addActor<1, ActorTest>(), true);
            }
            main.start();
            main.join();
            return 0;
        });

        test<100>("ChainEvent 4 Unlinked Core", [nb_actor]() {
            cube::Main<PhysicalCore<0>, PhysicalCore<1>, PhysicalCore<2>, PhysicalCore<3>> main;
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<0, ActorTest>(
                        main.addActor<1, ActorTest>(
                                main.addActor<2, ActorTest>(
                                        main.addActor<3, ActorTest>())), true);
            }

            main.start();
            main.join();
            return 0;
        });

        test<100>("ChainEvent 2/2 Linked Core", [nb_actor]() {
            cube::Main<CoreLink<PhysicalCore<0>, PhysicalCore<1>>, CoreLink<PhysicalCore<2>, PhysicalCore<3>>> main;
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<0, ActorTest>(
                        main.addActor<1, ActorTest>(
                                main.addActor<2, ActorTest>(
                                        main.addActor<3, ActorTest>())), true);
            }

            main.start();
            main.join();
            return 0;
        });
        return 0;
    });
}

int main() {
    nanolog::initialize(nanolog::GuaranteedLogger(), "./", "test-chain-actor.log", 1024);
    nanolog::set_log_level(nanolog::LogLevel::INFO);
    test_chain(1);
    test_chain(34);
    test_chain(55);
    test_chain(100);

}