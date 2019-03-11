#include <qb/main.h>
#include "MyActor.h"


int main (int, char *argv[]) {
    // (optional) initialize the logger
    qb::io::log::init(argv[0]); // filepath
    qb::io::log::setLevel(qb::io::log::Level::WARN); // log only warning, error an critical
    // usage
    LOG_INFO << "I will not be logged :(";

    // configure the Engine
    // Note : I will use only the core 0 and 1
    qb::Main main({0, 1});

    // First way to add actors at start
    main.addActor<MyActor>(0); // in Core id=0, default constructed
    main.addActor<MyActor>(1, 1337, 7331); // in Core id=1, constructed with parameters

    // Other way to add actors retrieving core builder
    main.core(0)
            .addActor<MyActor>()
            .addActor<MyActor>(1337, 7331);

    main.start();  // start the engine asynchronously
    main.join();   // Wait for the running engine
    // if all my actors had been destroyed then it will release the wait !
    return 0;
}