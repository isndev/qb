#include "assert.h"
#include "cube.h"

struct ChainEvent : cube::Event
{
    cube::ActorId first;
    uint64_t creation_time;
    uint64_t loop = 0;
};

class ActorTest : public cube::Actor {
    const bool first;
    const cube::ActorId to_send;

public:
    ActorTest(cube::ActorId const id = {}, bool first = false)
            : first(first)
            , to_send(id) {}

    bool onInit() override final {
        this-> template registerEvent<ChainEvent>(*this);
        if (first) {
            auto &event = this-> template push<ChainEvent>(to_send);
            event.first = this->id();
            event.creation_time = this->time();
        }
        return true;
    }

    void on(ChainEvent &event) const {
        if (event.loop >= 10000) {
            this->kill();
            if (!to_send)
                LOG_INFO << "Event Time To Arrive " << this->time() - event.creation_time << "ns";
        }
        if (first)
            event.creation_time = this->time();
        if (!to_send) {
            ++event.loop;
        }
        this->forward(to_send ? to_send : event.first, event);

    }
};

using namespace cube;
void test_chain(int nb_actor) {
    test("Test ChainEvent " + std::to_string(nb_actor) + " Actor(s) per Core 1000 chain loop\n",
    [nb_actor](auto &timer) {
        test<100>("ChainEvent 2 Unlinked Core", [nb_actor](auto &timer) {
            Cube main({0, 3});
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<ActorTest>(0, main.addActor<ActorTest>(3), true);
            }
            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("ChainEvent 2 Linked Core", [nb_actor](auto &timer) {
            Cube main({0, 1});
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<ActorTest>(0, main.addActor<ActorTest>(1), true);
            }
            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        test<100>("ChainEvent 4 Core", [nb_actor](auto &timer) {
            Cube main({0, 1, 2, 3});
            for (int i =0; i < nb_actor; ++i) {
                main.addActor<ActorTest>(0,
                        main.addActor<ActorTest>(1,
                                main.addActor<ActorTest>(2,
                                        main.addActor<ActorTest>(3))), true);
            }

            main.start();
            timer.reset();
            main.join();
            return 0;
        });

        return 0;
    });
}

int main(int argc, char *argv[]) {
    cube::io::log::init("./", argv[0]);
    cube::io::log::setLevel(cube::io::log::Level::WARN);

    test_chain(1);
    test_chain(34);
    test_chain(55);
    test_chain(100);

}
