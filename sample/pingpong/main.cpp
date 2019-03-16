/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

// main.cpp file
#include <qb/main.h>
#include "PingPongActor.h"

int main (int, char *argv[]) {
    // (optional) initialize the qb logger
    qb::io::log::init(argv[0]); // filename

    // configure the Core
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