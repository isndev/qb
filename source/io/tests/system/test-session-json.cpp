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
#include <filesystem>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/json.h>
#include <thread>

using namespace qb::io;

constexpr const std::size_t NB_ITERATION          = 4096;
constexpr const char        STRING_MESSAGE[]      = "Here is my content test";
constexpr unsigned short    JSON_TCP_PORT         = 22991;
constexpr unsigned short    JSON_SECURE_TCP_PORT  = 22992;
constexpr unsigned short    JSON_MALFORMED_PORT   = 22993;
std::atomic<std::size_t>    msg_count_server_side = 0;
std::atomic<std::size_t>    msg_count_client_side = 0;

namespace {

std::filesystem::path
ssl_resource_path(const char *file_name) {
    return std::filesystem::path(__FILE__).parent_path() / "resources" / "ssl" /
           file_name;
}

} // namespace

bool
all_done() {
    return msg_count_server_side == (NB_ITERATION) &&
           msg_count_client_side == NB_ITERATION;
}

template <typename Predicate>
bool
pump_until(Predicate &&predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        async::run(EVRUN_NOWAIT);
        if (std::chrono::steady_clock::now() >= deadline)
            return predicate();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// OVER TCP

class TestServer;

class TestServerClient : public use<TestServerClient>::tcp::client<TestServer> {
public:
    using Protocol = qb::protocol::json<TestServerClient>;

    explicit TestServerClient(IOServer &server)
        : client(server) {}

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
    void
    on(IOSession &) {
        ++connection_count;
    }

    [[nodiscard]] std::size_t
    connectionCount() const noexcept {
        return connection_count;
    }
};

class TestClient : public use<TestClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::json<TestClient>;

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
    ASSERT_EQ(server.transport().listen_v4(JSON_TCP_PORT), 0);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        ASSERT_EQ(client.transport().connect(uri{"tcp://127.0.0.1:" +
                                                 std::to_string(JSON_TCP_PORT)}),
                  SocketStatus::Done);
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client.publish(qb::json{{"message", STRING_MESSAGE}}, '\0');
        }

        EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    t.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), 1u);
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
    void
    on(IOSession &) {
        ++connection_count;
    }

    [[nodiscard]] std::size_t
    connectionCount() const noexcept {
        return connection_count;
    }
};

class TestSecureClient : public use<TestSecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::json_packed<TestSecureClient>;

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
    const auto cert_file = ssl_resource_path("cert.pem");
    const auto key_file  = ssl_resource_path("key.pem");
    ASSERT_TRUE(std::filesystem::exists(cert_file));
    ASSERT_TRUE(std::filesystem::exists(key_file));

    TestSecureServer server;
    server.transport().init(ssl::create_server_context(
        SSLv23_server_method(), cert_file.string(), key_file.string()));
    ASSERT_EQ(server.transport().listen_v6(JSON_SECURE_TCP_PORT), 0);
    server.start();

    std::thread t([]() {
        async::init();
        TestSecureClient client;
        if (SocketStatus::Done !=
            client.transport().connect_v6("::1", JSON_SECURE_TCP_PORT)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client << qb::json::to_msgpack(qb::json{{"message", STRING_MESSAGE}})
                   << '\0';
        }

        EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    t.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), 1u);
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
    ASSERT_EQ(server.transport().listen_v4(JSON_MALFORMED_PORT), 0);
    server.start();

    std::thread t([]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", JSON_MALFORMED_PORT),
                  qb::io::SocketStatus::Done);

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
