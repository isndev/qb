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

#include "Session.h"
#include "ServerActor.h"
#include <iostream>
#include <string_view>

Session::Session(ServerActor &server)
    : client(server) {
    // set session timeout to 10 secondes
    setTimeout(10);
}

// client is receiving a new message
void Session::on(IOMessage message, std::size_t size) {
    // handle your message here
    std::cout << "Received from Session(" << in().ident() << ") ip(" << in().getRemoteAddress()
              << ")" << std::endl
              << "-> Message (" << size << "): " << std::string_view(message, size - 1)
              << std::endl;
    // stream the message received to all connected sessions
    server().stream(message, size);
    // reset session time out
    updateTimeout();
}

// client is receving timeout
void Session::on(qb::io::async::event::timer &event) {
    // disconnect session on timeout
    // add reason for timeout
    disconnect(DisconnectedReason::ByTimeout);
}

// client is being disconnected
void Session::on(qb::io::async::event::disconnected const &event) {
    std::cout << "Session(" << in().ident() << ") ip(" << in().getRemoteAddress()
              << ") disconnected -> ";
    switch (event.reason) {
    case DisconnectedReason::ByUser:
        std::cout << "By User" << std::endl;
        break;
    case DisconnectedReason::ByTimeout:
        std::cout << "By Timeout" << std::endl;
        break;
    default:
        break;
    }
}