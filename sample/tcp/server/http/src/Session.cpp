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
    : client(server)
    , _file(prot().out_buffer()) {
    // set session timeout to 3 secondes
    setTimeout(60);
}

// client is receiving a new message
void
Session::on(IOMessage const &&message) {
    // handle your message here
    std::cout << "Received from Session(" << transport().ident() << ") ip("
              << transport().getRemoteAddress() << ")" << std::endl
              << "-> Message (" << message.size
              << "): " << std::string_view(message.data, message.size) << std::endl;
    // reset session time out
    updateTimeout();
    if (!server().router().route(*this, message.http))
        publish(server().router().getDefaultResponse());
}

// client is receving timeout
void
Session::on(qb::io::async::event::timer const &event) {
    // disconnect session on timeout
    // add reason for timeout
    disconnect(DisconnectedReason::ByTimeout);
}

// client write buffer is empty
void
Session::on(qb::io::async::event::pending_write const &) {
    updateTimeout();
}
// client write buffer has pending bytes
void
Session::on(qb::io::async::event::eos const &) {
    if (_file.is_open()) {
        if (_file.eof())
            disconnect(FileTransmitted);
        else if (_file.read() > 0) {
            ready_to_write();
        } else
            disconnect(ServerError);
    } else
        disconnect(ResponseTransmitted);
}

// client is being disconnected
void
Session::on(qb::io::async::event::disconnected const &event) {
    std::cout << "Session(" << transport().ident() << ") ip(" << transport().getRemoteAddress()
              << ") disconnected -> ";
    switch (event.reason) {
    case DisconnectedReason::ByUser:
        std::cout << "By User" << std::endl;
        break;
    case DisconnectedReason::ByTimeout:
        std::cout << "By Timeout" << std::endl;
        break;
    case ResponseTransmitted:
        std::cout << "Response Transmitted" << std::endl;
        break;
    case FileTransmitted:
        std::cout << "File Transmitted" << std::endl;
        break;
    case ServerError:
        std::cout << "Server error" << std::endl;
        break;
    default:
        break;
    }
}