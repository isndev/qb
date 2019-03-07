// main.cpp file
#include <cube/main.h>
#include "PingPongActor.h"

int main (int, char *argv[]) {
    // (optional) initialize the cube logger
    qb::io::log::init(argv[0]); // filename

    // configure the Engine
    // Note : I will use only the core 0 and 1
    qb::Main main({0, 1});

    // Build Pong Actor to core 0 and retrieve its unique identifier
    auto id_pong = main.addActor<PingPongActor>(0); // default constructed
    // Build Ping Actor to core 1 with Pong id as parameter
    main.addActor<PingPongActor>(1, id_pong); // constructed with parameters

    main.start();  // start the engine asynchronously
    main.join();   // wait for the running engine
    // if all my actors had been destroyed then it will release the wait
    return 0;
}