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
#include "Session.h"
#include <iostream>
#include <utility>

ServerActor::ServerActor(std::string iface, uint16_t port) noexcept
    : _iface(std::move(iface))
    , _port(port) {}

// Actor initialization
bool
ServerActor::onInit() {
    if (qb::io::SocketStatus::Done == transport().listen(_port, _iface)) {
        std::cout << "Server started listening on " << _iface << ":" << _port
                  << std::endl;
        start(); // register io to qb::io::listener
        return true;
    }

    return false;
}

// Called from qb::io on new session connected
void
ServerActor::on(Session &session) {
    std::cout << "Session(" << session.transport().ident() << ") "
              << "ip(" << session.transport().getRemoteAddress() << ") connected" << std::endl;
}

// Called from qb::io on server disconnected
void
ServerActor::on(qb::io::async::event::disconnected const &) {
    kill(); // kill ServerActor
}