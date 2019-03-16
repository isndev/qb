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

// PongActor.h file
#include <qb/actor.h>
#include "MyEvent.h"
#ifndef PONGACTOR_H_
# define PONGACTOR_H_

class PongActor
        : public qb::Actor // /!\ should inherit from qb actor
{
public:
    // /!\ never call any qb::Actor functions in constructor
    // /!\ use onInit function
    PongActor() = default;

    // /!\ the engine will call this function before adding PongActor
    bool onInit() override final {
        registerEvent<MyEvent>(*this);         // will just listen MyEvent

        return true;                           // init ok
    }
    // will call this function when PongActor receives MyEvent
    void on(MyEvent &event) {
        // debug print
        qb::io::cout() << "PongActor id(" << id() << ") received MyEvent" << std::endl;
        reply(event); // reply the event to SourceActor
        // debug print
        qb::io::cout() << "PongActor id(" << id() << ") has replied MyEvent" << std::endl;
        kill(); // then notify engine to kill PongActor
    }
};

#endif