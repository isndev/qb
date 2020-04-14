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

#include "../ws.h"
#include <atomic>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <thread>

using namespace qb::io;

constexpr const std::size_t NB_ITERATION = 4096;
constexpr const char STRING_MESSAGE[] = "Here is my content test";
std::atomic<std::size_t> msg_count_server_side = 0;
std::atomic<std::size_t> msg_count_client_side = 0;

bool
all_done() {
    return msg_count_server_side == (NB_ITERATION + 1) &&
           msg_count_client_side == (NB_ITERATION + 1);
}

// OVER TCP

class TestServer;

class TestServerClient : public use<TestServerClient>::tcp::client<TestServer> {
public:
    using Protocol = qb::http::protocol_view<TestServerClient>;
    using WS_Protocol = qb::http::ws::protocol<TestServerClient>;

    explicit TestServerClient(IOServer &server)
        : client(server) {}

    ~TestServerClient() {
        EXPECT_EQ(msg_count_server_side, NB_ITERATION + 1);
    }

    void
    on(Protocol::request &&event) {
        if (!this->switch_protocol<WS_Protocol>(*this, event.http))
            disconnect();
        else
            ++msg_count_server_side;
    }

    void
    on(WS_Protocol::message &&event) {
        EXPECT_EQ(STRING_MESSAGE, std::string(event.data, event.size));
        event.ws.masked = false;
        *this << event.ws;
        ++msg_count_server_side;
    }
};

class TestServer : public use<TestServer>::tcp::server<TestServerClient> {
    std::size_t connection_count = 0u;

public:
    ~TestServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void
    on(IOSession &) {
        ++connection_count;
    }
};

class TestClient : public use<TestClient>::tcp::client<> {
    const std::string ws_key;

public:
    using Protocol = qb::http::protocol_view<TestClient>;
    using WS_Protocol = qb::http::ws::protocol<TestClient>;

    TestClient()
        : ws_key(qb::http::ws::generateKey()) {}

    void
    sendWSHandshake() {
        qb::http::WebSocketRequest r(ws_key);
        r.path = "/";
        *this << r;
    }

    ~TestClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION + 1);
    }

    void
    on(Protocol::response &&event) {
        if (!this->switch_protocol<WS_Protocol>(*this, event.http, ws_key))
            disconnect();
        else {
            for (auto i = 0u; i < NB_ITERATION; ++i) {
                qb::http::ws::MessageText new_event;
                new_event << STRING_MESSAGE;
                *this << new_event;
            }
            ++msg_count_client_side;
        }
    }

    void
    on(WS_Protocol::message &&event) {
        EXPECT_EQ(STRING_MESSAGE, std::string(event.data, event.size));
        ++msg_count_client_side;
    }
};

TEST(Session, WEBSOCKET_OVER_TCP) {
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestServer server;
    server.transport().listen(60123);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        if (SocketStatus::Done != client.transport().connect("127.0.0.1", 60123)) {
            throw std::runtime_error("could not connect");
        }
        client.start();
        client.sendWSHandshake();

        for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
            async::run(EVRUN_ONCE);
    });

    for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

// OVER SECURE TCP

#ifdef QB_IO_WITH_SSL

class TestSecureServer;

class TestSecureServerClient
    : public use<TestSecureServerClient>::tcp::ssl::client<TestSecureServer> {

public:
    using Protocol = qb::http::protocol_view<TestSecureServerClient>;
    using WS_Protocol = qb::http::ws::protocol<TestSecureServerClient>;

    explicit TestSecureServerClient(IOServer &server)
        : client(server) {}

    ~TestSecureServerClient() {
        EXPECT_EQ(msg_count_server_side, NB_ITERATION + 1);
    }

    void
    on(Protocol::request &&event) {
        if (!this->switch_protocol<WS_Protocol>(*this, event.http))
            disconnect();
        else
            ++msg_count_server_side;
    }

    void
    on(WS_Protocol::message &&event) {
        EXPECT_EQ(STRING_MESSAGE, std::string(event.data, event.size));
        event.ws.masked = false;
        *this << event.ws;
        ++msg_count_server_side;
    }
};

class TestSecureServer
    : public use<TestSecureServer>::tcp::ssl::server<TestSecureServerClient> {
    std::size_t connection_count = 0u;

public:
    ~TestSecureServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void
    on(IOSession &) {
        ++connection_count;
    }
};

class TestSecureClient : public use<TestSecureClient>::tcp::ssl::client<> {
    const std::string ws_key;

public:
    using Protocol = qb::http::protocol_view<TestSecureClient>;
    using WS_Protocol = qb::http::ws::protocol<TestSecureClient>;

    TestSecureClient()
        : ws_key(qb::http::ws::generateKey()) {}

    void
    sendWSHandshake() {
        qb::http::WebSocketRequest r(ws_key);
        r.path = "/";
        *this << r;
    }

    ~TestSecureClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION + 1);
    }

    void
    on(Protocol::response &&event) {
        if (!this->switch_protocol<WS_Protocol>(*this, event.http, ws_key))
            disconnect();
        else {
            for (auto i = 0u; i < NB_ITERATION; ++i) {
                qb::http::ws::MessageText new_event;
                new_event << STRING_MESSAGE;
                *this << new_event;
            }
            ++msg_count_client_side;
        }
    }

    void
    on(WS_Protocol::message &&event) {
        EXPECT_EQ(STRING_MESSAGE, std::string(event.data, event.size));
        ++msg_count_client_side;
    }
};

TEST(Session, WEBSOCKET_OVER_SECURE_TCP) {
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestSecureServer server;
    server.transport().init(
        ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.transport().listen(60123);
    server.start();

    std::thread t([]() {
        async::init();
        TestSecureClient client;
        if (SocketStatus::Done != client.transport().connect("127.0.0.1", 60123)) {
            throw std::runtime_error("could not connect");
        }
        client.start();
        client.sendWSHandshake();

        for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
            async::run(EVRUN_ONCE);
    });

    for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

#endif