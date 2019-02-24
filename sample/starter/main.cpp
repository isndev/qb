#include "MyActor.h"
#include <cube/actor/main.h>

int main (int, char *argv[]) {
    // (optional) initialize the logger
    cube::io::log::init(argv[0]); // filepath
    cube::io::log::setLevel(cube::io::log::Level::WARN); // log only warning, error an critical
    // usage
    LOG_INFO << "I will not be logged :(";

    // configure the Engine
    // Note : I will use only the core 0 and 1
    cube::Main main({0, 1});

    // My start sequence -> add MyActor to core 0 and 1
    main.addActor<MyActor>(0); // default constructed
    main.addActor<MyActor>(1, 1337, 7331); // constructed with parameters

    main.start();  // start the engine asynchronously
    main.join();   // Wait for the running engine
    // if all my actors had been destroyed then it will release the wait !
    return 0;
}