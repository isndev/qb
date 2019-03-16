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

#include <qb/main.h>
#include "ActorProducer.h"
#include "ActorConsumer.h"

int main (int, char *argv[]) {
    // (optional) initialize the logger
    qb::io::log::init(argv[0]); // filepath
    qb::io::log::setLevel(qb::io::log::Level::INFO);

    // configure the Core
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

    qb::io::cout() << "Program is running Ctrl-C to stop" << std::endl;
    main.start();  // start the engine asynchronously
    main.join();   // Wait for the running engine
    qb::io::cout() << "Program has stopped" << std::endl;
    return 0;
}