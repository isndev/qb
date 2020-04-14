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

#include "CmdActor.h"
#include <iostream>

// constructor
CmdActor::CmdActor(qb::ActorId client_id) noexcept
    : _client_pipe(getPipe(client_id)) {
    // register callback
    registerCallback(*this);
}

// called each core loop
void CmdActor::onCallback() {
    std::string cmd;
    // /!\ blocking core, but it's ok for the example
    // CmdActor is alone in its core
    if (std::getline(std::cin, cmd))
        _client_pipe.push<CommandEvent>().message = cmd; // push line to client actor
    else {
        _client_pipe.push<qb::KillEvent>(); // push event to kill client actor
        kill();                             // kill cmd Actor
    }
}