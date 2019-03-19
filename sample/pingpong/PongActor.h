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
#ifndef PONGACTOR_H_
#define PONGACTOR_H_
#include <qb/actor.h>
#include "PingPongEvent.h"

class PongActor
        : public qb::Actor
{
public:
    PongActor() = default;

    bool onInit() override final {
        registerEvent<PingPongEvent>(*this);

        return true;
    }
	
    void on(PingPongEvent &event) {
        reply(event); 
    }
};

#endif