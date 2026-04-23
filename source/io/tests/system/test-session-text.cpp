/**
 * @file qb/io/tests/system/test-session-text.cpp
 * @brief Unit tests for text-based session protocols
 *
 * This file contains tests for the text-based communication protocols in the QB
 * framework, including command-based protocols over TCP, Unix sockets, secure TCP, and
 * UDP connections. It verifies correct session establishment, message transmission, and
 * session cleanup.
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
#include <filesystem>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <thread>

using namespace qb::io;

constexpr const std::size_t NB_ITERATION          = 1000;
constexpr const std::size_t NB_CLIENTS            = 5;
constexpr const char        STRING_MESSAGE[]      = "Here is my content test";
constexpr const char        UNIX_SOCK_PATH[]      = "qb-test.sock";
constexpr unsigned short    COMMAND_TCP_PORT      = 21991;
constexpr unsigned short    COMMAND_SECURE_TCP_PORT = 21992;
constexpr unsigned short    BINARY16_TCP_PORT     = 21993;
constexpr unsigned short    PROTO_SWITCH_TCP_PORT = 21994;
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

bool
client_received_expected_messages() {
    return msg_count_client_side >= NB_ITERATION;
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
    using Protocol = qb::protocol::text::command<TestServerClient>;

    explicit TestServerClient(IOServer &server)
        : client(server) {}

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
    using Protocol = qb::protocol::text::command<TestClient>;

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
    ASSERT_EQ(server.transport().listen_v4(COMMAND_TCP_PORT), 0);
    server.start();

    std::thread t([]() {
        async::init();
        TestClient client;
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", COMMAND_TCP_PORT),
                  SocketStatus::Done);
        client.start();

        for (auto i = 0u; i < NB_ITERATION; ++i) {
            client << STRING_MESSAGE << '\n';
        }

        EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    t.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), 1u);
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

        while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!all_done()));
    t.join();
}

#endif

// OVER SECURE TCP

#ifdef QB_HAS_SSL

class TestSecureServer;

class TestSecureServerClient
    : public use<TestSecureServerClient>::tcp::ssl::client<TestSecureServer> {
public:
    using Protocol = qb::protocol::text::command_view<TestSecureServerClient>;
    explicit TestSecureServerClient(IOServer &server)
        : client(server) {}

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
    using Protocol = qb::protocol::text::command<TestSecureClient>;

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(STRING_MESSAGE) - 1);
        ++msg_count_client_side;
    }
};

TEST(Session, COMMAND_OVER_SECURE_TCP) {
    async::init();
    msg_count_server_side = 0;
    const auto cert_file = ssl_resource_path("cert.pem");
    const auto key_file  = ssl_resource_path("key.pem");
    ASSERT_TRUE(std::filesystem::exists(cert_file));
    ASSERT_TRUE(std::filesystem::exists(key_file));

    TestSecureServer server;
    server.transport().init(ssl::create_server_context(
        SSLv23_server_method(), cert_file.string(), key_file.string()));
    ASSERT_EQ(server.transport().listen_v4(COMMAND_SECURE_TCP_PORT), 0);
    server.start();

    std::thread tc([]() {
        async::init();
        for (auto i = 0uz; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            if (SocketStatus::Done !=
                client.transport().connect_v4("127.0.0.1", COMMAND_SECURE_TCP_PORT)) {
                throw std::runtime_error("could not connect");
            }
            client.start();

            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client << STRING_MESSAGE << '\n';
            }

            EXPECT_TRUE(pump_until(client_done, std::chrono::seconds(10)));
        }
    });

    EXPECT_TRUE(pump_until([]() { return server_done() && client_done(); },
                           std::chrono::seconds(10)));
    tc.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION * NB_CLIENTS);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), NB_CLIENTS);
}

#ifndef _WIN32

TEST(Session, COMMAND_OVER_SECURE_UTCP) {
    unlink(UNIX_SOCK_PATH);
    async::init();
    msg_count_server_side = 0;
    const auto cert_file = ssl_resource_path("cert.pem");
    const auto key_file  = ssl_resource_path("key.pem");
    ASSERT_TRUE(std::filesystem::exists(cert_file));
    ASSERT_TRUE(std::filesystem::exists(key_file));

    TestSecureServer server;
    server.transport().init(ssl::create_server_context(
        SSLv23_server_method(), cert_file.string(), key_file.string()));
    server.transport().listen_un(UNIX_SOCK_PATH);
    server.start();
    std::thread tc([]() {
        async::init();
        for (auto i = 0uz; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            if (SocketStatus::Done != client.transport().connect_un(UNIX_SOCK_PATH)) {
                throw std::runtime_error("could not connect");
            }
            client.start();

            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client << STRING_MESSAGE << '\n';
            }

            while (async::run(EVRUN_NOWAIT) > 0 || (!client_done()));
        }
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!server_done() || !client_done()));
    tc.join();
}

#endif

#endif

// ---------------------------------------------------------------------------
// Binary protocol (basic_binary16) — exercises the OOB read fix
// ---------------------------------------------------------------------------

static std::atomic<std::size_t> bin_msg_count_server{0};
static std::atomic<std::size_t> bin_msg_count_client{0};

constexpr std::size_t BIN_ITERATIONS = 500;

bool bin_all_done() {
    return bin_msg_count_server >= BIN_ITERATIONS &&
           bin_msg_count_client >= BIN_ITERATIONS;
}

class BinaryServer;

class BinaryServerSession
    : public use<BinaryServerSession>::tcp::client<BinaryServer> {
public:
    using Protocol = qb::protocol::text::binary16<BinaryServerSession>;

    explicit BinaryServerSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.size, sizeof(STRING_MESSAGE) - 1);
        EXPECT_EQ(std::string_view(msg.data, msg.size), STRING_MESSAGE);

        uint16_t len = htons(static_cast<uint16_t>(msg.size));
        *this << std::string_view(reinterpret_cast<const char *>(&len), sizeof(len))
              << std::string_view(msg.data, msg.size);
        ++bin_msg_count_server;
    }
};

class BinaryServer
    : public use<BinaryServer>::tcp::server<BinaryServerSession> {
public:
    void on(IOSession &) {}
};

class BinaryClient : public use<BinaryClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::binary16<BinaryClient>;

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.size, sizeof(STRING_MESSAGE) - 1);
        EXPECT_EQ(std::string_view(msg.data, msg.size), STRING_MESSAGE);
        ++bin_msg_count_client;
    }
};

TEST(Session, BINARY16_OVER_TCP) {
    async::init();
    bin_msg_count_server = 0;
    bin_msg_count_client = 0;

    BinaryServer server;
    ASSERT_EQ(server.transport().listen_v4(BINARY16_TCP_PORT), 0);
    server.start();

    std::thread t([]() {
        async::init();
        BinaryClient client;
        if (SocketStatus::Done !=
            client.transport().connect_v4("127.0.0.1", BINARY16_TCP_PORT)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        for (auto i = 0uz; i < BIN_ITERATIONS; ++i) {
            uint16_t len = htons(static_cast<uint16_t>(sizeof(STRING_MESSAGE) - 1));
            client.publish(std::string_view(reinterpret_cast<const char *>(&len), sizeof(len)),
                           std::string_view(STRING_MESSAGE, sizeof(STRING_MESSAGE) - 1));
        }

        while (async::run(EVRUN_NOWAIT) > 0 || !bin_all_done());
    });

    while (async::run(EVRUN_NOWAIT) > 0 || !bin_all_done());
    t.join();

    EXPECT_EQ(bin_msg_count_server, BIN_ITERATIONS);
    EXPECT_EQ(bin_msg_count_client, BIN_ITERATIONS);
}

// ---------------------------------------------------------------------------
// Protocol switch mid-session — text -> binary16
// ---------------------------------------------------------------------------

static std::atomic<std::size_t> switch_text_count{0};
static std::atomic<std::size_t> switch_bin_count{0};

class ProtoSwitchServer;

class ProtoSwitchSession
    : public use<ProtoSwitchSession>::tcp::client<ProtoSwitchServer> {
public:
    using Protocol = qb::protocol::text::command<ProtoSwitchSession>;

    explicit ProtoSwitchSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        ++switch_text_count;
        if (msg.text == "SWITCH") {
            switch_protocol<qb::protocol::text::binary16<ProtoSwitchSession>>(
                static_cast<ProtoSwitchSession &>(*this));
        } else {
            *this << msg.text << Protocol::end;
        }
    }

    void
    on(qb::protocol::text::binary16<ProtoSwitchSession>::message &&msg) {
        ++switch_bin_count;
        uint16_t len = htons(static_cast<uint16_t>(msg.size));
        *this << std::string_view(reinterpret_cast<const char *>(&len), sizeof(len))
              << std::string_view(msg.data, msg.size);
    }
};

class ProtoSwitchServer
    : public use<ProtoSwitchServer>::tcp::server<ProtoSwitchSession> {
public:
    void on(IOSession &) {}
};

TEST(Session, PROTOCOL_SWITCH_TEXT_TO_BINARY) {
    async::init();
    switch_text_count = 0;
    switch_bin_count = 0;

    ProtoSwitchServer server;
    ASSERT_EQ(server.transport().listen_v4(PROTO_SWITCH_TCP_PORT), 0);
    server.start();

    std::atomic<bool> done{false};
    std::thread t([&done]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", PROTO_SWITCH_TCP_PORT),
                  SocketStatus::Done);

        sock.write("hello\n", 6);
        char buf[512]{};
        sock.set_nonblocking(false);
        auto n = sock.read(buf, sizeof(buf));
        EXPECT_GT(n, 0);
        if (n > 0)
            EXPECT_EQ(std::string_view(buf, 5), "hello");

        sock.write("SWITCH\n", 7);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const char payload[] = "binary_data!";
        uint16_t len = htons(static_cast<uint16_t>(sizeof(payload) - 1));
        sock.write(reinterpret_cast<const char *>(&len), sizeof(len));
        sock.write(payload, sizeof(payload) - 1);

        char bin_buf[512]{};
        n = sock.read(bin_buf, sizeof(bin_buf));
        EXPECT_GT(n, 0);
        if (n > 2) {
            uint16_t got_len = ntohs(*reinterpret_cast<uint16_t *>(bin_buf));
            EXPECT_EQ(got_len, sizeof(payload) - 1);
            EXPECT_EQ(std::string_view(bin_buf + 2, got_len), payload);
        }

        sock.disconnect();
        done = true;
    });

    for (auto i = 0; i < 100 && !done; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    t.join();

    EXPECT_GE(switch_text_count, 1u);
    EXPECT_GE(switch_bin_count, 1u);
}

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

TEST(Session, DISABLED_COMMAND_OVER_UDP) {
    msg_count_server_side = 0;

    async::init();
    TestUDPServerClient server;
    server.transport().bind_v4(9999);
    server.start();

    std::thread tc([]() {
        async::init();

        for (auto i = 0uz; i < 5uz; ++i) {
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

            while (async::run(EVRUN_NOWAIT) > 0 || (!client_done()));
        }
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!server_done() || !client_done()));
    tc.join();
}
