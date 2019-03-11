#include <qb/main.h>
#include "ActorProducer.h"
#include "ActorConsumer.h"

int main (int, char *argv[]) {
    // (optional) initialize the logger
    qb::io::log::init(argv[0]); // filepath
    qb::io::log::setLevel(qb::io::log::Level::INFO);

    // configure the Engine
    // Note : I will use only the core 0 and 1
    qb::Main main({0, 1});

    const auto nb_consumer = 100;
    const auto nb_producer = 100;

    auto builder0 = main.core(0);
    for (int i = 0; i < nb_consumer; ++i)
        builder0.addActor<ActorConsumer>();
    // check if builder has add all consumers
    if (builder0) {
        auto builder1 = main.core(1);
        for (int i = 0; i < nb_producer; ++i)
            builder1.addActor<ActorProducer>(builder0.idList());
        if (!builder1)
            return 1;
    } else
        return 1;

    main.start();  // start the engine asynchronously
    std::getchar();
    main.stop();
    main.join();   // Wait for the running engine
    return 0;
}