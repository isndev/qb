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
#include <qb/io/async/quic.h>
#include <qb/io/protocol/text.h>
#include <thread>

using namespace qb::io;

// Per-TEST() wall time: run this binary with --gtest_print_time=1 (Google Test).

constexpr const std::size_t NB_ITERATION          = 4096;
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
        if (async::listener::current.nb_invoked_event() == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
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

        EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until(all_done, std::chrono::seconds(10)));
    t.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), 1u);
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

    void on(qb::io::async::event::disconnected const &ev) {
        std::fprintf(stderr,
                     "[UTCP DIAG] server-session disconnected reason=%d sysErr=%d msgServerRecv=%zu pendingWrite=%zu\n",
                     ev.reason, this->system_error(), msg_count_server_side.load(),
                     static_cast<std::size_t>(this->has_pending_write() ? 1 : 0));
        std::fflush(stderr);
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

    void on(qb::io::async::event::disconnected const &ev) {
        std::fprintf(stderr,
                     "[UTCP DIAG] client disconnected reason=%d sysErr=%d msgRecv=%zu pendingWrite=%zu\n",
                     ev.reason, this->system_error(), msg_count_client_side.load(),
                     static_cast<std::size_t>(this->has_pending_write() ? 1 : 0));
        std::fflush(stderr);
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
        for (std::size_t i = 0; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            // Self-signed test certificate on 127.0.0.1: opt out of the
            // secure-by-default peer verification for this local fixture.
            client.transport().set_insecure();
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
        for (std::size_t i = 0; i < NB_CLIENTS; ++i) {
            msg_count_client_side = 0;
            TestSecureClient client;
            // Self-signed test certificate over a Unix-domain socket: opt out
            // of the secure-by-default peer verification for this local fixture.
            client.transport().set_insecure();
            if (SocketStatus::Done != client.transport().connect_un(UNIX_SOCK_PATH)) {
                throw std::runtime_error("could not connect");
            }
            client.start();

            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client << STRING_MESSAGE << '\n';
            }

            if (!pump_until(client_done, std::chrono::seconds(10))) {
                std::fprintf(stderr,
                             "[UTCP DIAG] client thread iter %zu timeout: client=%zu/%zu server=%zu (expected %zu)\n",
                             i, msg_count_client_side.load(), NB_ITERATION,
                             msg_count_server_side.load(), NB_ITERATION * (i + 1));
                std::fflush(stderr);
                ADD_FAILURE();
            }
        }
    });

    if (!pump_until([]() { return server_done() && client_done(); },
                    std::chrono::seconds(10))) {
        std::fprintf(stderr,
                     "[UTCP DIAG] main timeout: server=%zu/%zu client=%zu/%zu\n",
                     msg_count_server_side.load(), NB_ITERATION * NB_CLIENTS,
                     msg_count_client_side.load(), NB_ITERATION);
        std::fflush(stderr);
        ADD_FAILURE();
    }
    tc.join();
    EXPECT_EQ(msg_count_server_side.load(), NB_ITERATION * NB_CLIENTS);
    EXPECT_EQ(msg_count_client_side.load(), NB_ITERATION);
    EXPECT_GE(server.connectionCount(), NB_CLIENTS);
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

        for (std::size_t i = 0; i < BIN_ITERATIONS; ++i) {
            uint16_t len = htons(static_cast<uint16_t>(sizeof(STRING_MESSAGE) - 1));
            client.publish(std::string_view(reinterpret_cast<const char *>(&len), sizeof(len)),
                           std::string_view(STRING_MESSAGE, sizeof(STRING_MESSAGE) - 1));
        }

        EXPECT_TRUE(pump_until(bin_all_done, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until(bin_all_done, std::chrono::seconds(10)));
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

    // Do not use EVRUN_ONCE here: with timerfd-based libev, a lone ev_io workload can block
    // EVRUN_ONCE for an extremely long waittime (see async::run_once() docs).
    EXPECT_TRUE(pump_until([&] { return done.load(); }, std::chrono::seconds(5)))
        << "client thread should finish (protocol switch + disconnect)";

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
    server.transport().bind_v4(21995);
    server.start();

    std::thread tc([]() {
        async::init();

        for (std::size_t i = 0; i < 5u; ++i) {
            msg_count_client_side = 0;
            TestUDPClient client;
            client.transport().init();
            if (!client.transport().is_open()) {
                throw std::runtime_error("could not connect");
            }
            client.start();
            for (auto j = 0u; j < NB_ITERATION; ++j) {
                client.setDestination(endpoint().as_in("127.0.0.1", 21995));
                client << STRING_MESSAGE << '\n';
            }

            while (async::run(EVRUN_NOWAIT) > 0 || (!client_done()));
        }
    });

    while (async::run(EVRUN_NOWAIT) > 0 || (!server_done() || !client_done()));
    tc.join();
}

// OVER QUIC

class TextQuicSession : public use<TextQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<TextQuicSession>;

    std::string last_message;

    explicit TextQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.text);
    }
};

class EchoTextQuicSession : public use<EchoTextQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<EchoTextQuicSession>;

    std::size_t messages = 0;

    explicit EchoTextQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        *this << message.text << Protocol::end;
        ++messages;
    }
};

class BinaryQuicSession : public use<BinaryQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::binary16<BinaryQuicSession>;

    std::string last_message;

    explicit BinaryQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.data, message.size);
    }
};

class SwitchQuicSession : public use<SwitchQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<SwitchQuicSession>;

    std::size_t text_messages = 0;
    std::size_t binary_messages = 0;
    std::string last_binary;

    explicit SwitchQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        ++text_messages;
        if (message.text == "SWITCH") {
            this->template switch_protocol<
                qb::protocol::text::binary16<SwitchQuicSession>>(
                static_cast<SwitchQuicSession &>(*this));
        }
    }

    void
    on(qb::protocol::text::binary16<SwitchQuicSession>::message &&message) {
        ++binary_messages;
        last_binary.assign(message.data, message.size);
    }
};

class CloseAfterDeliverQuicSession
    : public use<CloseAfterDeliverQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<CloseAfterDeliverQuicSession>;

    explicit CloseAfterDeliverQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(Protocol::message &&message) {
        *this << message.text << Protocol::end;
        close_after_deliver();
    }
};

class RawQuicSession : public use<RawQuicSession>::quic::session {
public:
    std::size_t pending_read_events = 0;
    std::size_t last_pending_read = 0;

    explicit RawQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}

    void
    on(async::event::pending_read &&event) {
        ++pending_read_events;
        last_pending_read = event.bytes;
    }
};

class QuicDrainProbe
    : public async::quic::io_handler<QuicDrainProbe, TextQuicSession> {
public:
    bool
    feed(async::quic::event::stream_data const &event) {
        return feed_stream_data(event);
    }

    bool
    feed_with_credit(async::quic::event::stream_data const &event,
                     std::uint64_t &credited) {
        return feed_stream_data(event,
                                [&credited](std::uint64_t, std::uint64_t,
                                            std::uint64_t bytes) {
                                    credited += bytes;
                                });
    }

    template <typename Session>
    void
    drain(Session &session, std::size_t &sent) {
        drain_stream_output(session,
                            [&sent](std::uint64_t, std::uint64_t,
                                    std::span<const std::byte> data, bool) {
                                sent += data.size();
                            });
    }

    template <typename Session, typename Send>
    void
    drain_with(Session &session, Send &&send) {
        drain_stream_output(session, std::forward<Send>(send));
    }
};

class ServerOwnedQuicProbe;

class ServerOwnedTextQuicSession
    : public use<ServerOwnedTextQuicSession>::quic::client<ServerOwnedQuicProbe> {
public:
    using Protocol = qb::protocol::text::command<ServerOwnedTextQuicSession>;

    std::string last_message;

    explicit ServerOwnedTextQuicSession(ServerOwnedQuicProbe &server)
        : client(server) {}

    void
    on(Protocol::message &&message) {
        last_message.assign(message.text);
    }
};

class ServerOwnedQuicProbe
    : public async::quic::io_handler<ServerOwnedQuicProbe,
                                     ServerOwnedTextQuicSession> {
public:
    bool
    feed(async::quic::event::stream_data const &event) {
        return feed_stream_data(event);
    }
};

template <typename StreamSession>
class ProtocolQuicServer
    : public async::quic::server<ProtocolQuicServer<StreamSession>, StreamSession> {
public:
    int connected = 0;

    void
    on(async::quic::event::connected const &) {
        ++connected;
    }
};

class TextProtocolQuicClient
    : public use<TextProtocolQuicClient>::quic::connector<TextQuicSession> {};

template <typename Predicate>
bool
pump_quic_until(Predicate &&predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        async::run(EVRUN_NOWAIT);
        if (std::chrono::steady_clock::now() >= deadline)
            return predicate();
        if (async::listener::current.nb_invoked_event() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

template <typename Server, typename Client>
void
connect_local_quic_pair(Server &server, Client &client) {
    ASSERT_TRUE(server.listen(uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"),
                              ssl_resource_path("key.pem"), {"qb-test"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    const auto endpoint_uri = std::string{"quic://127.0.0.1:"} +
                              std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(uri{endpoint_uri}, client_tls, {"qb-test"}));

    ASSERT_TRUE(pump_quic_until([&] {
        return server.current_state() == async::quic::endpoint::state::connected &&
               client.current_state() == async::quic::endpoint::state::connected;
    }, std::chrono::seconds(3)));
}

TEST(Session, COMMAND_OVER_QUIC) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    async::init();

    ProtocolQuicServer<EchoTextQuicSession> server;
    TextProtocolQuicClient client;
    connect_local_quic_pair(server, client);

    auto stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), "hello ", false);
    client.send_stream_data(stream.id(), "quic\n", true);

    EXPECT_TRUE(pump_quic_until([&] {
        auto *response = client.stream_session(stream.id());
        return response && response->last_message == "hello quic";
    }, std::chrono::seconds(3)));

    auto *server_session = server.stream_session(stream.id());
    auto *client_session = client.stream_session(stream.id());
    ASSERT_NE(server_session, nullptr);
    ASSERT_NE(client_session, nullptr);
    EXPECT_EQ(server_session->messages, 1u);
    EXPECT_EQ(client_session->last_message, "hello quic");

    client.close();
    server.close();
    async::listener::current.clear();
#endif
}

TEST(Session, BINARY16_OVER_QUIC) {
    BinaryQuicSession session{0};
    const std::string payload = "binary-over-quic";
    uint16_t len = htons(static_cast<uint16_t>(payload.size()));

    session.append(std::string_view(reinterpret_cast<char const *>(&len), sizeof(len)));
    EXPECT_TRUE(session.process());
    EXPECT_TRUE(session.last_message.empty());

    session.append(payload);
    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.last_message, payload);
    EXPECT_EQ(session.pendingRead(), 0u);
}

TEST(Session, PROTOCOL_SWITCH_TEXT_TO_BINARY_OVER_QUIC) {
    SwitchQuicSession session{0};
    session.append("SWITCH\n");
    ASSERT_TRUE(session.process());
    EXPECT_EQ(session.text_messages, 1u);

    const std::string payload = "after-switch";
    uint16_t len = htons(static_cast<uint16_t>(payload.size()));
    session.append(std::string_view(reinterpret_cast<char const *>(&len), sizeof(len)));
    session.append(payload);

    ASSERT_TRUE(session.process());
    EXPECT_EQ(session.binary_messages, 1u);
    EXPECT_EQ(session.last_binary, payload);
}

TEST(Session, CLOSE_AFTER_DELIVER_FLUSHES_QUIC_INPUT_AND_KEEPS_OUTPUT) {
    CloseAfterDeliverQuicSession session{0};

    session.append("bye\n");

    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.pendingRead(), 0u);
    EXPECT_EQ(session.pendingWrite(), 4u);
    EXPECT_EQ(std::string_view(session.out().begin(), session.out().size()), "bye\n");
    EXPECT_FALSE(session.protocol()->ok());
}

TEST(Session, RAW_QUIC_SESSION_KEEPS_PENDING_INPUT_WITHOUT_PROTOCOL) {
    RawQuicSession session{0};

    session.append("raw-bytes");

    EXPECT_TRUE(session.process());
    EXPECT_EQ(session.pendingRead(), 9u);
    EXPECT_EQ(session.pending_read_events, 1u);
    EXPECT_EQ(session.last_pending_read, 9u);
}

TEST(Session, QUIC_READ_CAP_OVERFLOW_FAILS_PROTOCOL_PROCESSING) {
    TextQuicSession session{0};
    session.set_max_read_buffer_size(4);

    EXPECT_FALSE(session.append("hello"));
    EXPECT_EQ(session.pendingRead(), 0u);

    EXPECT_FALSE(session.process());
    EXPECT_EQ(session.disconnection_reason(),
              static_cast<int>(async::event::disconnect_reason::buffer_overflow));
}

TEST(Session, QUIC_WRITE_CAP_OVERFLOW_REJECTS_PUBLISH) {
    TextQuicSession session{0};
    session.set_max_write_buffer_size(4);

    EXPECT_EQ(session.publish("hello", std::size_t{5}), nullptr);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.disconnection_reason(),
              static_cast<int>(async::event::disconnect_reason::buffer_overflow));
}

TEST(Session, QUIC_DRAIN_ACCOUNTS_WRITTEN_BYTES_AFTER_BACKEND_ACCEPTS_OUTPUT) {
    TextQuicSession session{0};
    QuicDrainProbe handler;
    std::size_t sent = 0;

    session << "typed" << TextQuicSession::Protocol::end;
    ASSERT_EQ(session.bytes_written(), 0u);

    handler.drain(session, sent);

    EXPECT_EQ(sent, 6u);
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.out().begin(), session.out().data());
    EXPECT_EQ(session.bytes_written(), 6u);
}

TEST(Session, QUIC_OUTPUT_PIPE_REORDERS_WHEN_DRAIN_IS_REENTERED) {
    TextQuicSession session{0};
    QuicDrainProbe handler;
    std::vector<std::string> chunks;
    bool appended = false;

    session << "first" << TextQuicSession::Protocol::end;

    handler.drain_with(
        session, [&](std::uint64_t, std::uint64_t, std::span<const std::byte> data, bool) {
            chunks.emplace_back(reinterpret_cast<char const *>(data.data()), data.size());
            if (!appended) {
                appended = true;
                session << "second" << TextQuicSession::Protocol::end;
            }
        });

    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "first\n");
    EXPECT_EQ(chunks[1], "second\n");
    EXPECT_EQ(session.pendingWrite(), 0u);
    EXPECT_EQ(session.out().begin(), session.out().data());
}

TEST(Session, QUIC_SESSIONS_ARE_KEYED_BY_CONNECTION_AND_STREAM_ID) {
    QuicDrainProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    auto *first = handler.session(10, 0);
    auto *second = handler.session(11, 0);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(first->connection_id(), 10u);
    EXPECT_EQ(second->connection_id(), 11u);
    EXPECT_EQ(first->last_message, "first");
    EXPECT_EQ(second->last_message, "second");
    EXPECT_EQ(handler.session_count(), 2u);
}

TEST(Session, QUIC_CONNECTION_CLOSE_ONLY_CLEARS_MATCHING_CONNECTION_STREAMS) {
    QuicDrainProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({10, 4, "first-other\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    handler.clearSessions(10);

    EXPECT_EQ(handler.session(10, 0), nullptr);
    EXPECT_EQ(handler.session(10, 4), nullptr);
    auto *remaining = handler.session(11, 0);
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->last_message, "second");
    EXPECT_EQ(handler.session_count(), 1u);
}

TEST(Session, QUIC_SESSION_DISPOSE_REMOVES_MATCHING_CONNECTION_ONLY) {
    ServerOwnedQuicProbe handler;

    ASSERT_TRUE(handler.feed({10, 0, "first\n", true}));
    ASSERT_TRUE(handler.feed({11, 0, "second\n", true}));

    auto *first = handler.session(10, 0);
    ASSERT_NE(first, nullptr);
    first->dispose();

    EXPECT_EQ(handler.session(10, 0), nullptr);
    auto *remaining = handler.session(11, 0);
    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->last_message, "second");
    EXPECT_EQ(handler.session_count(), 1u);
}

TEST(Session, QUIC_FLOW_CREDIT_IS_RETURNED_ONLY_AFTER_PROTOCOL_CONSUMES_BYTES) {
    QuicDrainProbe handler;
    std::uint64_t credited = 0;

    ASSERT_TRUE(handler.feed_with_credit({10, 0, "hello", false}, credited));
    auto *session = handler.session(10, 0);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->pendingRead(), 5u);
    EXPECT_EQ(credited, 0u);

    ASSERT_TRUE(handler.feed_with_credit({10, 0, " world\n", true}, credited));
    EXPECT_EQ(session->pendingRead(), 0u);
    EXPECT_EQ(session->last_message, "hello world");
    EXPECT_EQ(credited, 12u);
}

TEST(Session, MANY_COMMAND_STREAMS_OVER_ONE_QUIC_CONNECTION) {
#ifndef QB_HAS_QUIC
    GTEST_SKIP() << "QUIC support is disabled";
#else
    if (!std::filesystem::exists(ssl_resource_path("cert.pem")) ||
        !std::filesystem::exists(ssl_resource_path("key.pem")))
        GTEST_SKIP() << "Test SSL certificates are not available";

    async::init();

    ProtocolQuicServer<EchoTextQuicSession> server;
    TextProtocolQuicClient client;
    connect_local_quic_pair(server, client);

    std::vector<std::uint64_t> stream_ids;
    constexpr std::size_t stream_count = 16;
    stream_ids.reserve(stream_count);
    for (std::size_t i = 0; i < stream_count; ++i) {
        auto stream = client.open_bidirectional_stream();
        stream_ids.push_back(stream.id());
        auto payload = std::string{"stream-"} + std::to_string(i) + "\n";
        client.send_stream_data(stream.id(), payload, true);
    }

    EXPECT_TRUE(pump_quic_until([&] {
        for (std::size_t i = 0; i < stream_ids.size(); ++i) {
            auto *session = client.stream_session(stream_ids[i]);
            if (!session || session->last_message != std::string{"stream-"} + std::to_string(i))
                return false;
        }
        return true;
    }, std::chrono::seconds(5)));

    EXPECT_EQ(server.session_count(), stream_count);
    EXPECT_EQ(client.session_count(), stream_count);

    client.close();
    server.close();
    async::listener::current.clear();
#endif
}
