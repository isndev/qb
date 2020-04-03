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

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/protocol/cmd.h>
#include "event/CommandEvent.h"
#include "event/RetryConnectEvent.h"

#ifndef QB_SAMPLE_PROJECT_CLIENTACTOR_H
#define QB_SAMPLE_PROJECT_CLIENTACTOR_H

class ClientActor
    : public qb::Actor
    , public qb::io::use<ClientActor>::tcp::client<qb::io::protocol::cmd> {

    bool connect();

    const std::string _ip;
    const uint16_t _port;

public:

    ClientActor() = delete;
    ClientActor(std::string const& ip, uint16_t port) noexcept;

    bool onInit() final;

    // new message received
    void on(IOMessage message, std::size_t size);
    // client is disconnected
    void on(qb::io::async::event::disconnected const& event);

    // new message from command actor
    void on(CommandEvent & event);

    // retry session connected
    void on(RetryConnectEvent const& event);
};

#endif //QB_SAMPLE_PROJECT_CLIENTACTOR_H
