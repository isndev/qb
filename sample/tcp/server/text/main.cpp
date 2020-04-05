/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#include "ServerActor.h"
#include <qb/main.h>

int main(int argc, char *argv[]) {
    // default listenning
    std::string iface = "127.0.0.1";
    uint16_t port = 60123;
    // usage: ./qb-sample-server-text [IFACE] [PORT]
    if (argc > 1)
        iface = argv[1];
    if (argc > 2)
        port = std::atoi(argv[2]);

    qb::Main main;
    main.core(0).addActor<ServerActor>(iface, port);

    main.start(false);
    main.join();
    return 0;
}
