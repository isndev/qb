#include "assert.h"
#include "cube.h"

using namespace qb;
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
    qb::io::log::init("./", argv[0]);
    qb::io::log::setLevel(qb::io::log::Level::WARN);

    test_chain(1);
    test_chain(34);
    test_chain(55);
    test_chain(100);

}
