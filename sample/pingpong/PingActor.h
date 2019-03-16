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

// PingActor.h file
#include <qb/actor.h>
#include "MyEvent.h"
#ifndef PINGACTOR_H_
# define PINGACTOR_H_

class PingActor
        : public qb::Actor // /!\ should inherit from qb actor
{
    const qb::ActorId _id_pong; // Pong ActorId
public:
    PingActor() = delete; // PingActor requires PongActor Actorid
    // /!\ never call any qb::Actor functions in constructor
    // /!\ use onInit function
    explicit PingActor(const qb::ActorId id_pong)
            : _id_pong(id_pong) {}

    // /!\ the engine will call this function before adding PingPongActor
    bool onInit() override final {
        registerEvent<MyEvent>(*this);         // will listen MyEvent
        auto &event = push<MyEvent>(_id_pong); // push MyEvent to PongActor and keep a reference to the event
        event.data = 1337;                     // set trivial data
        event.container.push_back(7331);       // set dynamic data

        // debug print
        qb::io::cout() << "PingActor id(" << id() << ") has sent MyEvent" << std::endl;
        return true;                           // init ok
    }
    // will call this function when PingActor receives MyEvent
    void on(MyEvent &) {
        // debug print
        qb::io::cout() << "PingActor id(" << id() << ") received MyEvent" << std::endl;
        kill(); // then notify engine to kill PingActor
    }
};

#endif