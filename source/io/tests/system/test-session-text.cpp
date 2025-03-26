/**
 * @file qb/io/tests/system/test-session-text.cpp
 * @brief Unit tests for text-based session protocols
 * 
 * This file contains tests for the text-based communication protocols in the QB framework,
 * including command-based protocols over TCP, Unix sockets, secure TCP, and UDP connections.
 * It verifies correct session establishment, message transmission, and session cleanup.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Tests
 */

#include <atomic>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <thread>

using namespace qb::io;

constexpr const std::size_t NB_ITERATION = 1000;
constexpr const std::size_t NB_CLIENTS = 5;
constexpr const char STRING_MESSAGE[] = "Here is my content test";
constexpr const char UNIX_SOCK_PATH[] = "qb-test.sock";
std::atomic<std::size_t> msg_count_server_side = 0;
std::atomic<std::size_t> msg_count_client_side = 0;

bool
all_done() {
    return msg_count_server_side == (NB_ITERATION) &&
           msg_count_client_side == (NB_ITERATION);
}

bool
server_done() {
    return msg_count_server_side == (NB_ITERATION * NB_CLIENTS);
}

bool
client_done() {
    return msg_count_client_side == NB_ITERATION;
}

// OVER TCP

class TestServer;

class TestServerClient : public use<TestServerClient>::tcp::client<TestServer> {
public:
    using Protocol = qb::protocol::text::command<TestServerClient>;

    explicit TestServerClient(IOServer &server)
        : client(server) {}

    ~TestServerClient() {
        EXPECT_EQ(msg_count_server_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        *this << msg.text << Protocol::end;
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
    using Protocol = qb::protocol::text::command<TestClient>;

    ~TestClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, COMMAND_OVER_TCP) {
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestServer server;
    server.transport().listen_v4(9999);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        if (SocketStatus::Done != client.transport().connect_v4("127.0.0.1", 9999)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client << STRING_MESSAGE << '\n';
        }

        for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
            async::run(EVRUN_ONCE);
    });

    for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

#ifndef _WIN32

TEST(Session, COMMAND_OVER_UTCP) {
    unlink(UNIX_SOCK_PATH);
    async::init();
    msg_count_server_side = 0;
    msg_count_client_side = 0;

    TestServer server;
    server.transport().listen_un(UNIX_SOCK_PATH);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        if (SocketStatus::Done != client.transport().connect_un(UNIX_SOCK_PATH)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client << STRING_MESSAGE << '\n';
        }

        for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
            async::run(EVRUN_ONCE);
    });

    for (auto i = 0; i < (NB_ITERATION * 5) && !all_done(); ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

#endif

// OVER SECURE TCP

#ifdef QB_IO_WITH_SSL

class TestSecureServer;

class TestSecureServerClient
    : public use<TestSecureServerClient>::tcp::ssl::client<TestSecureServer> {
public:
    using Protocol = qb::protocol::text::command_view<TestSecureServerClient>;
    explicit TestSecureServerClient(IOServer &server)
        : client(server) {}

    ~TestSecureServerClient() {
        EXPECT_EQ(msg_count_server_side % NB_ITERATION, 0);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        *this << msg.text << Protocol::end;
        ++msg_count_server_side;
    }
};

class TestSecureServer
    : public use<TestSecureServer>::tcp::ssl::server<TestSecureServerClient> {
    std::size_t connection_count = 0u;

public:
    ~TestSecureServer() {
        EXPECT_EQ(connection_count, NB_CLIENTS);
    }

    void
    on(IOSession &) {
        ++connection_count;
    }
};

class TestSecureClient : public use<TestSecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<TestSecureClient>;

    ~TestSecureClient() {
        EXPECT_EQ(msg_count_client_side % NB_ITERATION, 0);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, COMMAND_OVER_SECURE_TCP) {
    async::init();
    msg_count_server_side = 0;

    TestSecureServer server;
    server.transport().init(
        ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.transport().listen_v4(9999);
    server.start();

    std::thread tc([]() {
        async::init();
        for (auto i = 0; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            if (SocketStatus::Done !=
                client.transport().connect_v4("127.0.0.1", 9999)) {
                throw std::runtime_error("could not connect");
            }
            client.start();

            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client << STRING_MESSAGE << '\n';
            }

            for (auto j = 0; j < (NB_ITERATION * 5) && !client_done(); ++j)
                async::run(EVRUN_ONCE);
        }
    });

    for (auto i = 0;
         i < (NB_ITERATION * NB_CLIENTS) && (!server_done() || !client_done()); ++i)
        async::run(EVRUN_ONCE);
    tc.join();
}

#    ifndef _WIN32

TEST(Session, COMMAND_OVER_SECURE_UTCP) {
    unlink(UNIX_SOCK_PATH);
    async::init();
    msg_count_server_side = 0;

    TestSecureServer server;
    server.transport().init(
        ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.transport().listen_un(UNIX_SOCK_PATH);
    server.start();
    std::thread tc([]() {
        async::init();
        for (auto i = 0; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            if (SocketStatus::Done != client.transport().connect_un(UNIX_SOCK_PATH)) {
                throw std::runtime_error("could not connect");
            }
            client.start();

            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client << STRING_MESSAGE << '\n';
            }

            for (auto j = 0; j < (NB_ITERATION * 5) && !client_done(); ++j)
                async::run(EVRUN_ONCE);
        }
    });

    for (auto i = 0;
         i < (NB_ITERATION * NB_CLIENTS) && (!server_done() || !client_done()); ++i)
        async::run(EVRUN_ONCE);
    tc.join();
}

#    endif

#endif

// OVER UDP

class TestUDPServerClient : public use<TestUDPServerClient>::udp::server {
public:
    using Protocol = qb::protocol::text::command<TestUDPServerClient>;

    TestUDPServerClient() = default;
    ~TestUDPServerClient() {
        EXPECT_EQ(msg_count_server_side % NB_ITERATION, 0);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        *this << msg.text << Protocol::end;
        ++msg_count_server_side;
    }
};

class TestUDPClient : public use<TestUDPClient>::udp::client {
public:
    using Protocol = qb::protocol::text::command<TestUDPClient>;

    TestUDPClient() = default;
    ~TestUDPClient() {
        EXPECT_EQ(msg_count_client_side, NB_ITERATION);
    }

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, COMMAND_OVER_UDP) {
    msg_count_server_side = 0;

    async::init();
    TestUDPServerClient server;
    server.transport().bind_v4(9999);
    server.start();

    std::thread tc([]() {
        async::init();

        for (auto i = 0; i < 5; ++i) {
            msg_count_client_side = 0;
            TestUDPClient client;
            client.transport().init();
            if (!client.transport().is_open()) {
                throw std::runtime_error("could not connect");
            }
            client.start();
            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client.setDestination(endpoint().as_in("127.0.0.1", 9999));
                client << STRING_MESSAGE << '\n';
            }

            for (auto j = 0; i < (NB_ITERATION * 10000) && !client_done(); ++j)
                async::run(EVRUN_ONCE);
        }
    });

    for (auto i = 0;
         i < (NB_ITERATION * 10000 * 5) && (!server_done() || !client_done()); ++i)
        async::run(EVRUN_ONCE);
    tc.join();
}
