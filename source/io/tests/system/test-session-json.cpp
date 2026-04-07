/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
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

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/json.h>
#include <thread>

using namespace qb::io;

constexpr const std::size_t NB_ITERATION          = 4096;
constexpr const char        STRING_MESSAGE[]      = "Here is my content test";
std::atomic<std::size_t>    msg_count_server_side = 0;
std::atomic<std::size_t>    msg_count_client_side = 0;

bool
all_done() {
    return msg_count_server_side == (NB_ITERATION) &&
           msg_count_client_side == NB_ITERATION;
}

// OVER TCP

class TestServer;

class TestServerClient : public use<TestServerClient>::tcp::client<TestServer> {
public:
    using Protocol = qb::protocol::json<TestServerClient>;

    explicit TestServerClient(IOServer &server)
        : client(server) {}

    ~TestServerClient() {
        EXPECT_EQ(msg_count_server_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(),
                  sizeof(STRING_MESSAGE) - 1);
        publish(msg.json, '\0');
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
public:
    using Protocol = qb::protocol::json<TestClient>;

    ~TestClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(),
                  sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, JSON_OVER_TCP) {
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestServer server;
    server.transport().listen_v4(9999);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        if (SocketStatus::Done !=
            client.transport().connect(uri{"tcp://localhost:9999"})) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client.publish(qb::json{{"message", STRING_MESSAGE}}, '\0');
        }

        while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    t.join();
}

// OVER SECURE TCP

#ifdef QB_HAS_SSL

class TestSecureServer;

class TestSecureServerClient
    : public use<TestSecureServerClient>::tcp::ssl::client<TestSecureServer> {
public:
    using Protocol = qb::protocol::json_packed<TestSecureServerClient>;

    explicit TestSecureServerClient(IOServer &server)
        : client(server) {}

    ~TestSecureServerClient() {
        EXPECT_EQ(msg_count_server_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(),
                  sizeof(STRING_MESSAGE) - 1);
        *this << qb::json::to_msgpack(msg.json) << '\0';
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
public:
    using Protocol = qb::protocol::json_packed<TestSecureClient>;

    ~TestSecureClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.json["message"].get<std::string>().size(),
                  sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, JSON_OVER_SECURE_TCP) {
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestSecureServer server;
    server.transport().init(
        ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.transport().listen_v6(9999);
    server.start();

    std::thread t([]() {
        async::init();
        TestSecureClient client;
        if (SocketStatus::Done != client.transport().connect_v6("::1", 9999)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client << qb::json::to_msgpack(qb::json{{"message", STRING_MESSAGE}})
                   << '\0';
        }

        while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    t.join();
}

#endif

// ---------------------------------------------------------------------------
// Malformed JSON resilience — no crash/terminate on invalid input
// ---------------------------------------------------------------------------

class MalformedJsonServer;

class MalformedJsonSession
    : public use<MalformedJsonSession>::tcp::client<MalformedJsonServer> {
public:
    using Protocol = qb::protocol::json<MalformedJsonSession>;
    int good_messages = 0;
    int bad_messages = 0;

    explicit MalformedJsonSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        if (msg.json.is_discarded())
            ++bad_messages;
        else
            ++good_messages;
    }
};

class MalformedJsonServer
    : public use<MalformedJsonServer>::tcp::server<MalformedJsonSession> {
public:
    bool session_connected = false;
    bool session_disconnected = false;

    void on(IOSession &) { session_connected = true; }
    void on(qb::io::async::event::disconnected &&) { session_disconnected = true; }
};

TEST(Session, JSON_MALFORMED_RESILIENCE) {
    async::init();

    MalformedJsonServer server;
    server.transport().listen_v4(9998);
    server.start();

    std::thread t([]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", 9998), qb::io::SocketStatus::Done);

        const char good_json[] = "{\"key\":\"value\"}\0";
        sock.write(good_json, sizeof(good_json) - 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const char bad_json[] = "{this is not json}\0";
        sock.write(bad_json, sizeof(bad_json) - 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sock.disconnect();
    });

    for (auto i = 0; i < 50 && !server.session_disconnected; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    t.join();
    EXPECT_TRUE(server.session_connected);
}