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

#include <string_view>
#include "ClientActor.h"

ClientActor::ClientActor(std::string const& ip, uint16_t port) noexcept
    : _ip(ip), _port(port)
{
    setCoreLowLatency(false);
}

bool ClientActor::connect() {
    if (qb::io::SocketStatus::Done == in().connect(_ip, _port)) {
        start();
        return true;
    }
    push<RetryConnectEvent>(id());
    return false;
}

bool ClientActor::onInit() {
    registerEvent<CommandEvent>(*this);
    registerEvent<RetryConnectEvent>(*this);

    return connect();
}

// received new message
void ClientActor::on(char const* message, std::size_t size) {
    std::cout << "Received: " << std::string_view(message, size);
}

// on disconnect try to reconnect
void ClientActor::on(CommandEvent & event) {
    event.message[event.message.size()] = '\n';
    publish(event.message.c_str(), event.message.size() + 1);
}

// on disconnect try to reconnect
void ClientActor::on(RetryConnectEvent const& event) {
    connect();
}

void ClientActor::on(qb::io::async::event::disconnected const&) {
    push<RetryConnectEvent>(id());
}