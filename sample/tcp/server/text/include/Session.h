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

#ifndef QB_SAMPLE_PROJECT_SESSION_H
#define QB_SAMPLE_PROJECT_SESSION_H

#include <qb/io/async.h>
#include <qb/io/protocol/cmd.h>

class ServerActor;
class Session
    : public qb::io::use<Session>::tcp::client<qb::io::protocol::cmd, ServerActor>
    , public qb::io::use<Session>::timeout {
    // clean way to identify why client is disconnected
    enum DisconnectedReason : int {
        ByUser = 0,
        ByTimeout,
        Undefined // should never hapen
    };

public:
    Session() = delete;
    explicit Session(ServerActor &server);

    // client is receiving a new message
    void on(IOMessage msg, std::size_t size);
    // client is receiving timeout
    void on(qb::io::async::event::timeout &event);
    // client write buffer is empty
    void on(qb::io::async::event::eos const &) {}
    // client is being disconnected
    void on(qb::io::async::event::disconnected const &event);
};

#endif // QB_SAMPLE_PROJECT_SESSION_H
