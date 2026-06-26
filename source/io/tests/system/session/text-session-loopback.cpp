/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/session/text-session-loopback.cpp
 * @brief Text / binary line protocols over real loopback transports — the qb-io session system suite.
 *
 * This is the genuine *network* half harvested out of the misnamed `system/test-session-text.cpp`: every
 * case here spins up a real `qb::io::async` event loop, binds a real loopback socket, forks a client
 * thread, and pumps the loop. The pure in-process protocol/io_handler state-machine cases that used to
 * share that file now live in `unit/protocol/quic-protocol-statemachine.cpp`; this file owns only the
 * tests that actually move bytes across a transport.
 *
 * Structure and the de-flake / de-dup work applied (per dossier qbio-c08 §"Improve / add"):
 *   - The four near-identical `COMMAND_OVER_{TCP,UTCP,SECURE_TCP,SECURE_UTCP}` bodies are collapsed into a
 *     single `TYPED_TEST` over four transport policies. Each policy binds an EPHEMERAL port (TCP/TLS) or a
 *     per-PID Unix path — NO fixed ports anywhere — and knows how to listen/connect; the test body is
 *     written once.
 *   - The shared mutable `msg_count_server_side` / `msg_count_client_side` file-scope atomics are GONE.
 *     Each transport policy carries its OWN pair of counters (template-distinct static atomics), reset in
 *     the fixture SetUp, so the suite is no longer order-coupled and each transport's counts are isolated.
 *   - `pump_until` is the shared deadline-bounded loop pump (`coroutine_test_support.h`); the wall-clock
 *     10s budgets shrink to a single bounded helper that fails LOUDLY on timeout instead of hanging.
 *   - `PROTOCOL_SWITCH_TEXT_TO_BINARY` now asserts EXACT switch counts and drains the binary reply in a
 *     bounded loop (no `if (n>2)` skip), and the 100 ms `sleep_for` race is replaced by a
 *     `pump_until(server switched)` handshake.
 *   - `DISABLED_COMMAND_OVER_UDP` is fixed and re-enabled as `CommandOverUdp` with a bounded pump and its
 *     destructor-time assertions moved into the test body.
 *   - `NB_ITERATION` is dropped to a small fixed count (the bulk throughput is a benchmark concern, not a
 *     correctness one); each frame's payload length is still verified in every `on()` handler so a dropped
 *     or corrupted frame still fails.
 *   - The live QUIC loopback cases stay here (they need a real UDP socket + local TLS) behind a hard
 *     cert-presence assertion in a QUIC build, not a silent skip.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
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
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <qb/io/protocol/text.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/ssl_fixtures.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

constexpr std::size_t NB_ITERATION    = 64; // correctness count; throughput is owned by the benchmark
constexpr char        STRING_MESSAGE[] = "Here is my content test";
constexpr std::size_t MESSAGE_LEN     = sizeof(STRING_MESSAGE) - 1;

#ifndef _WIN32
// RAII per-PID Unix-domain socket path that is unlinked on construction and teardown.
struct unix_socket_file {
    explicit unix_socket_file(std::string_view suffix)
        : path("/tmp/qb-text-session-" + std::to_string(::getpid()) + "-" + std::string(suffix) + ".sock") {
        ::unlink(path.c_str());
    }
    unix_socket_file(unix_socket_file const &)            = delete;
    unix_socket_file &operator=(unix_socket_file const &) = delete;
    ~unix_socket_file() {
        ::unlink(path.c_str());
    }
    std::string path;
};
#endif

} // namespace

// =============================================================================
// PARAMETRISED COMMAND-OVER-<TRANSPORT> (TCP / UTCP / SECURE_TCP / SECURE_UTCP)
//
// Each transport is a "policy" carrying:
//   - the server/client session types (CRTP over the right transport mixin),
//   - its OWN server/client message counters (no shared globals),
//   - a listen(server) that binds an EPHEMERAL endpoint and returns a connect token,
//   - a connect(client, token), and capability/precondition gates.
// The single TYPED_TEST body drives NB_ITERATION echoes and asserts exact counts.
// =============================================================================

namespace {

// Counter pair owned per transport policy — template-distinct so each transport has isolated state.
template <typename Tag>
struct transport_counters {
    static inline std::atomic<std::size_t> server{0};
    static inline std::atomic<std::size_t> client{0};
    static void
    reset() {
        server.store(0);
        client.store(0);
    }
};

// ---- plain TCP ------------------------------------------------------------
struct TcpPolicy {
    using Tag      = TcpPolicy;
    using Counters = transport_counters<Tag>;

    class Server;
    class ServerSession : public use<ServerSession>::tcp::client<Server> {
    public:
        using Protocol = qb::protocol::text::command<ServerSession>;
        explicit ServerSession(IOServer &server)
            : client(server) {}
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            *this << msg.text << Protocol::end;
            ++Counters::server;
        }
    };
    class Server : public use<Server>::tcp::server<ServerSession> {
    public:
        std::size_t connections = 0;
        void
        on(IOSession &) {
            ++connections;
        }
    };
    class Client : public use<Client>::tcp::client<> {
    public:
        using Protocol = qb::protocol::text::command<Client>;
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            ++Counters::client;
        }
    };

    static bool
    precondition() {
        return true;
    }
    // Returns the ephemeral port the kernel assigned.
    static unsigned short
    listen(Server &server) {
        EXPECT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        return server.transport().local_endpoint().port();
    }
    static bool
    connect(Client &client, unsigned short port) {
        return client.transport().connect_v4("127.0.0.1", port) == SocketStatus::Done;
    }
};

#ifndef _WIN32
// ---- Unix-domain TCP ------------------------------------------------------
struct UtcpPolicy {
    using Tag      = UtcpPolicy;
    using Counters = transport_counters<Tag>;

    class Server;
    class ServerSession : public use<ServerSession>::tcp::client<Server> {
    public:
        using Protocol = qb::protocol::text::command<ServerSession>;
        explicit ServerSession(IOServer &server)
            : client(server) {}
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            *this << msg.text << Protocol::end;
            ++Counters::server;
        }
    };
    class Server : public use<Server>::tcp::server<ServerSession> {
    public:
        std::size_t connections = 0;
        void
        on(IOSession &) {
            ++connections;
        }
    };
    class Client : public use<Client>::tcp::client<> {
    public:
        using Protocol = qb::protocol::text::command<Client>;
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            ++Counters::client;
        }
    };

    static inline unix_socket_file *socket_path = nullptr; // set by the fixture for the active test

    static bool
    precondition() {
        return socket_path != nullptr;
    }
    static unsigned short
    listen(Server &server) {
        EXPECT_EQ(server.transport().listen_un(socket_path->path), SocketStatus::Done);
        return 0; // unix path carried out-of-band
    }
    static bool
    connect(Client &client, unsigned short) {
        return client.transport().connect_un(socket_path->path) == SocketStatus::Done;
    }
};
#endif

#ifdef QB_HAS_SSL
// ---- TLS over TCP ---------------------------------------------------------
struct SecureTcpPolicy {
    using Tag      = SecureTcpPolicy;
    using Counters = transport_counters<Tag>;

    class Server;
    class ServerSession : public use<ServerSession>::tcp::ssl::client<Server> {
    public:
        using Protocol = qb::protocol::text::command_view<ServerSession>;
        explicit ServerSession(IOServer &server)
            : client(server) {}
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            *this << msg.text << Protocol::end;
            ++Counters::server;
        }
    };
    class Server : public use<Server>::tcp::ssl::server<ServerSession> {
    public:
        std::size_t connections = 0;
        void
        on(IOSession &) {
            ++connections;
        }
    };
    class Client : public use<Client>::tcp::ssl::client<> {
    public:
        using Protocol = qb::protocol::text::command<Client>;
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            ++Counters::client;
        }
    };

    static bool
    precondition() {
        return qb::io::test::require_ssl_files();
    }
    static unsigned short
    listen(Server &server) {
        server.transport().init(ssl::create_server_context(SSLv23_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                           qb::io::test::ssl_resource_path("key.pem")));
        EXPECT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        return server.transport().local_endpoint().port();
    }
    static bool
    connect(Client &client, unsigned short port) {
        // Self-signed local certificate: opt out of secure-by-default peer verification.
        client.transport().set_insecure();
        return client.transport().connect_v4("127.0.0.1", port) == SocketStatus::Done;
    }
};

#ifndef _WIN32
// ---- TLS over Unix-domain TCP --------------------------------------------
struct SecureUtcpPolicy {
    using Tag      = SecureUtcpPolicy;
    using Counters = transport_counters<Tag>;

    class Server;
    class ServerSession : public use<ServerSession>::tcp::ssl::client<Server> {
    public:
        using Protocol = qb::protocol::text::command_view<ServerSession>;
        explicit ServerSession(IOServer &server)
            : client(server) {}
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            *this << msg.text << Protocol::end;
            ++Counters::server;
        }
    };
    class Server : public use<Server>::tcp::ssl::server<ServerSession> {
    public:
        std::size_t connections = 0;
        void
        on(IOSession &) {
            ++connections;
        }
    };
    class Client : public use<Client>::tcp::ssl::client<> {
    public:
        using Protocol = qb::protocol::text::command<Client>;
        void
        on(Protocol::message &&msg) {
            EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
            ++Counters::client;
        }
    };

    static inline unix_socket_file *socket_path = nullptr;

    static bool
    precondition() {
        return socket_path != nullptr && qb::io::test::require_ssl_files();
    }
    static unsigned short
    listen(Server &server) {
        server.transport().init(ssl::create_server_context(SSLv23_server_method(), qb::io::test::ssl_resource_path("cert.pem"),
                                                           qb::io::test::ssl_resource_path("key.pem")));
        EXPECT_EQ(server.transport().listen_un(socket_path->path), SocketStatus::Done);
        return 0;
    }
    static bool
    connect(Client &client, unsigned short) {
        client.transport().set_insecure();
        return client.transport().connect_un(socket_path->path) == SocketStatus::Done;
    }
};
#endif // !_WIN32
#endif // QB_HAS_SSL

} // namespace

template <typename Policy>
class TextCommandTransport : public ::testing::Test {
protected:
#ifndef _WIN32
    unix_socket_file socket_path_{::testing::UnitTest::GetInstance()->current_test_info()->name()};
#endif

    void
    SetUp() override {
        Policy::Counters::reset();
#ifndef _WIN32
        if constexpr (requires { Policy::socket_path; })
            Policy::socket_path = &socket_path_;
#endif
    }

    void
    TearDown() override {
#ifndef _WIN32
        if constexpr (requires { Policy::socket_path; })
            Policy::socket_path = nullptr;
#endif
    }
};

using TextTransports = ::testing::Types<TcpPolicy
#ifndef _WIN32
                                        ,
                                        UtcpPolicy
#endif
#ifdef QB_HAS_SSL
                                        ,
                                        SecureTcpPolicy
#ifndef _WIN32
                                        ,
                                        SecureUtcpPolicy
#endif
#endif
                                        >;
TYPED_TEST_SUITE(TextCommandTransport, TextTransports);

/**
 * @test A command-protocol echo round-trips over the transport
 * @brief The single body for all four transports: bind an ephemeral endpoint, connect a client on a
 *        worker thread, send NB_ITERATION `STRING_MESSAGE\n` lines, echo each server-side; assert both
 *        sides see exactly NB_ITERATION messages (each length-checked in its handler) and the server
 *        accepted at least one connection.
 */
TYPED_TEST(TextCommandTransport, CommandEchoRoundTrip) {
    using Policy = TypeParam;
    ASSERT_TRUE(Policy::precondition()) << "transport precondition not met (e.g. missing TLS certs in a QB_HAS_SSL build)";

    async::init();

    typename Policy::Server server;
    const unsigned short    port = Policy::listen(server);
    server.start();

    std::atomic<bool> client_thread_ok{false};
    std::thread       worker([&] {
        async::init();
        typename Policy::Client client;
        ASSERT_TRUE(Policy::connect(client, port));
        client.start();

        for (std::size_t i = 0; i < NB_ITERATION; ++i)
            client << STRING_MESSAGE << '\n';

        const bool ok = pump_until([&] { return Policy::Counters::client.load() == NB_ITERATION; }, std::chrono::seconds(10));
        EXPECT_TRUE(ok) << "client never received all echoes";
        client_thread_ok.store(ok);
    });

    EXPECT_TRUE(pump_until(
        [&] { return Policy::Counters::server.load() == NB_ITERATION && Policy::Counters::client.load() == NB_ITERATION; },
        std::chrono::seconds(10)))
        << "server/client did not converge on NB_ITERATION";

    worker.join();

    EXPECT_TRUE(client_thread_ok.load());
    EXPECT_EQ(Policy::Counters::server.load(), NB_ITERATION);
    EXPECT_EQ(Policy::Counters::client.load(), NB_ITERATION);
    EXPECT_GE(server.connections, 1u);

    async::listener::current.clear();
}

// =============================================================================
// BINARY16 LENGTH-PREFIXED ECHO OVER TCP (the OOB-read regression guard)
// =============================================================================

namespace {

std::atomic<std::size_t> bin_server{0};
std::atomic<std::size_t> bin_client{0};
constexpr std::size_t    BIN_ITERATIONS = 64;

class BinaryServer;
class BinaryServerSession : public use<BinaryServerSession>::tcp::client<BinaryServer> {
public:
    using Protocol = qb::protocol::text::binary16<BinaryServerSession>;
    explicit BinaryServerSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.size, MESSAGE_LEN);
        EXPECT_EQ(std::string_view(msg.data, msg.size), STRING_MESSAGE);
        const std::uint16_t len = htons(static_cast<std::uint16_t>(msg.size));
        *this << std::string_view(reinterpret_cast<const char *>(&len), sizeof(len)) << std::string_view(msg.data, msg.size);
        ++bin_server;
    }
};
class BinaryServer : public use<BinaryServer>::tcp::server<BinaryServerSession> {
public:
    void
    on(IOSession &) {}
};
class BinaryClient : public use<BinaryClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::binary16<BinaryClient>;
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.size, MESSAGE_LEN);
        EXPECT_EQ(std::string_view(msg.data, msg.size), STRING_MESSAGE);
        ++bin_client;
    }
};

} // namespace

/**
 * @test binary16 length-prefixed frames echo correctly over loopback TCP
 * @brief The client publishes a 16-bit length + payload BIN_ITERATIONS times; the server validates size
 *        and contents and echoes. Asserts both sides reach exactly BIN_ITERATIONS — the OOB-read fix guard.
 */
TEST(TextSessionBinary16, Binary16EchoOverTcp) {
    async::init();
    bin_server.store(0);
    bin_client.store(0);

    BinaryServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread worker([port] {
        async::init();
        BinaryClient client;
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        client.start();

        for (std::size_t i = 0; i < BIN_ITERATIONS; ++i) {
            const std::uint16_t len = htons(static_cast<std::uint16_t>(MESSAGE_LEN));
            client.publish(std::string_view(reinterpret_cast<const char *>(&len), sizeof(len)), std::string_view(STRING_MESSAGE, MESSAGE_LEN));
        }

        EXPECT_TRUE(pump_until([&] { return bin_server.load() >= BIN_ITERATIONS && bin_client.load() >= BIN_ITERATIONS; },
                               std::chrono::seconds(10)));
    });

    EXPECT_TRUE(pump_until([&] { return bin_server.load() >= BIN_ITERATIONS && bin_client.load() >= BIN_ITERATIONS; },
                           std::chrono::seconds(10)));
    worker.join();

    EXPECT_EQ(bin_server.load(), BIN_ITERATIONS);
    EXPECT_EQ(bin_client.load(), BIN_ITERATIONS);
    async::listener::current.clear();
}

// =============================================================================
// PROTOCOL SWITCH text -> binary16 mid-session (exact counts, no sleep race)
// =============================================================================

namespace {

std::atomic<std::size_t> switch_text{0};
std::atomic<std::size_t> switch_bin{0};
std::atomic<bool>        switch_done{false};

class ProtoSwitchServer;
class ProtoSwitchSession : public use<ProtoSwitchSession>::tcp::client<ProtoSwitchServer> {
public:
    using Protocol = qb::protocol::text::command<ProtoSwitchSession>;
    explicit ProtoSwitchSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&msg) {
        ++switch_text;
        if (msg.text == "SWITCH") {
            switch_protocol<qb::protocol::text::binary16<ProtoSwitchSession>>(static_cast<ProtoSwitchSession &>(*this));
            switch_done.store(true);
        } else {
            *this << msg.text << Protocol::end;
        }
    }
    void
    on(qb::protocol::text::binary16<ProtoSwitchSession>::message &&msg) {
        ++switch_bin;
        const std::uint16_t len = htons(static_cast<std::uint16_t>(msg.size));
        *this << std::string_view(reinterpret_cast<const char *>(&len), sizeof(len)) << std::string_view(msg.data, msg.size);
    }
};
class ProtoSwitchServer : public use<ProtoSwitchServer>::tcp::server<ProtoSwitchSession> {
public:
    void
    on(IOSession &) {}
};

// Read exactly `want` bytes from a blocking socket within a deadline; returns bytes read.
std::size_t
read_exact(qb::io::tcp::socket &sock, char *buf, std::size_t want) {
    std::size_t got      = 0;
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got < want && std::chrono::steady_clock::now() < deadline) {
        const auto n = sock.read(buf + got, want - got);
        if (n > 0)
            got += static_cast<std::size_t>(n);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return got;
}

} // namespace

/**
 * @test A session switches from the text command protocol to binary16 mid-stream
 * @brief A raw tcp::socket client sends `hello\n` (echoed), then `SWITCH\n`; the test pumps until the
 *        server has actually switched (no 100 ms sleep race), then sends one binary frame and reads the
 *        full echo back with a bounded read loop. Asserts EXACT switch counts (text==2: hello+SWITCH,
 *        bin==1) and the round-tripped binary payload.
 */
TEST(TextSessionProtocolSwitch, TextToBinary) {
    async::init();
    switch_text.store(0);
    switch_bin.store(0);
    switch_done.store(false);

    ProtoSwitchServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::atomic<bool>  worker_ok{false};
    constexpr char     payload[] = "binary_data!";
    constexpr std::size_t payload_len = sizeof(payload) - 1;

    std::thread worker([&] {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);

        // text round-trip. The server echoes `msg.text << Protocol::end`, i.e.
        // "hello" + '\n' (6 bytes) — read the FULL echoed line including the
        // terminator so no stray '\n' is left to corrupt the later binary frame.
        ASSERT_GT(sock.write("hello\n", 6), 0);
        sock.set_nonblocking(false);
        char echo[6]{};
        ASSERT_EQ(read_exact(sock, echo, 6), 6u);
        EXPECT_EQ(std::string_view(echo, 6), "hello\n");

        // trigger the protocol switch and wait for it (no wall-clock guess)
        ASSERT_GT(sock.write("SWITCH\n", 7), 0);
        EXPECT_TRUE(pump_until([&] { return switch_done.load(); }, std::chrono::seconds(5)))
            << "server never switched to binary16";

        // one binary frame: 16-bit length + payload
        const std::uint16_t len = htons(static_cast<std::uint16_t>(payload_len));
        ASSERT_GT(sock.write(reinterpret_cast<const char *>(&len), sizeof(len)), 0);
        ASSERT_GT(sock.write(payload, payload_len), 0);

        // read back the full echoed binary frame (length prefix + payload), unconditionally
        char     reply[2 + payload_len]{};
        const auto got = read_exact(sock, reply, sizeof(reply));
        ASSERT_EQ(got, sizeof(reply)) << "short read of the echoed binary frame";
        const std::uint16_t got_len = ntohs(*reinterpret_cast<std::uint16_t *>(reply));
        EXPECT_EQ(got_len, payload_len);
        EXPECT_EQ(std::string_view(reply + 2, got_len), payload);

        sock.disconnect();
        worker_ok.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return worker_ok.load(); }, std::chrono::seconds(8)))
        << "client thread did not complete the switch round-trip";
    worker.join();

    EXPECT_TRUE(worker_ok.load());
    EXPECT_EQ(switch_text.load(), 2u) << "expected exactly two text messages: hello + SWITCH";
    EXPECT_EQ(switch_bin.load(), 1u) << "expected exactly one binary message after the switch";
    async::listener::current.clear();
}

// =============================================================================
// COMMAND OVER UDP (re-enabled from DISABLED_, bounded pump, body assertions)
// =============================================================================

namespace {

std::atomic<std::size_t> udp_server{0};
std::atomic<std::size_t> udp_client{0};

class UdpServer : public use<UdpServer>::udp::server {
public:
    using Protocol = qb::protocol::text::command<UdpServer>;
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
        *this << msg.text << Protocol::end;
        ++udp_server;
    }
};
class UdpClient : public use<UdpClient>::udp::client {
public:
    using Protocol = qb::protocol::text::command<UdpClient>;
    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), MESSAGE_LEN);
        ++udp_client;
    }
};

} // namespace

/**
 * @test A command-protocol echo round-trips over UDP (datagram loopback)
 * @brief Re-enabled from the dead DISABLED_COMMAND_OVER_UDP: binds an ephemeral UDP server, sends
 *        NB_ITERATION lines from a single client to it, echoes each back. UDP is lossy, so the oracle is
 *        bounded by a deadline and asserts the server received at least one and the client received at
 *        least one echo (no unbounded spin, no hang on loss). Each frame is length-checked in its handler.
 */
TEST(TextSessionUdp, CommandOverUdp) {
    async::init();
    udp_server.store(0);
    udp_client.store(0);

    UdpServer server;
    ASSERT_EQ(server.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread worker([port] {
        async::init();
        UdpClient client;
        client.transport().init();
        ASSERT_TRUE(client.transport().is_open());
        client.start();

        for (std::size_t i = 0; i < NB_ITERATION; ++i) {
            client.setDestination(endpoint().as_in("127.0.0.1", port));
            client << STRING_MESSAGE << '\n';
        }

        // Datagrams can be dropped on loopback under load; require at least one round-trip, bounded.
        EXPECT_TRUE(pump_until([&] { return udp_client.load() >= 1; }, std::chrono::seconds(5)))
            << "client never received any UDP echo";
    });

    EXPECT_TRUE(pump_until([&] { return udp_server.load() >= 1; }, std::chrono::seconds(5)))
        << "server never received any UDP datagram";
    worker.join();

    EXPECT_GE(udp_server.load(), 1u);
    EXPECT_GE(udp_client.load(), 1u);
    async::listener::current.clear();
}

// =============================================================================
// LIVE QUIC LOOPBACK (real UDP + local TLS; QB_HAS_QUIC only, certs REQUIRED)
// =============================================================================

#ifdef QB_HAS_QUIC

namespace {

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

class TextClientQuicSession : public use<TextClientQuicSession>::quic::session {
public:
    using Protocol = qb::protocol::text::command<TextClientQuicSession>;
    std::string last_message;
    explicit TextClientQuicSession(std::uint64_t stream_id)
        : client(stream_id) {}
    void
    on(Protocol::message &&message) {
        last_message.assign(message.text);
    }
};

template <typename StreamSession>
class ProtocolQuicServer : public async::quic::server<ProtocolQuicServer<StreamSession>, StreamSession> {
public:
    int connected = 0;
    void
    on(async::quic::event::connected const &) {
        ++connected;
    }
};

class TextProtocolQuicClient : public use<TextProtocolQuicClient>::quic::connector<TextClientQuicSession> {};

template <typename Server, typename Client>
void
connect_local_quic_pair(Server &server, Client &client) {
    ASSERT_TRUE(server.listen(uri{"quic://127.0.0.1:0"}, qb::io::test::ssl_resource_path("cert.pem"),
                              qb::io::test::ssl_resource_path("key.pem"), {"qb-test"}));
    ASSERT_GT(server.local_endpoint().port(), 0);

    quic::tls_config client_tls;
    client_tls.server_name = "localhost";
    client_tls.verify_peer = false;

    const auto endpoint_uri = std::string{"quic://127.0.0.1:"} + std::to_string(server.local_endpoint().port());
    ASSERT_TRUE(client.connect(uri{endpoint_uri}, client_tls, {"qb-test"}));

    ASSERT_TRUE(pump_until(
        [&] {
            return server.current_state() == async::quic::endpoint::state::connected
                   && client.current_state() == async::quic::endpoint::state::connected;
        },
        std::chrono::seconds(5)))
        << "QUIC handshake did not complete";
}

} // namespace

/**
 * @test A command-protocol message round-trips over a single QUIC stream
 * @brief Establishes a local QUIC pair, opens a bidirectional stream, sends "hello quic\n" in two
 *        chunks, and asserts the echoed text reassembles to "hello quic" on the client and the server
 *        delivered exactly one message. Certs are REQUIRED in a QUIC build (hard fail, not silent skip).
 */
TEST(TextSessionQuic, CommandOverQuic) {
    ASSERT_TRUE(qb::io::test::require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    async::init();

    ProtocolQuicServer<EchoTextQuicSession> server;
    TextProtocolQuicClient                  client;
    connect_local_quic_pair(server, client);

    auto stream = client.open_bidirectional_stream();
    client.send_stream_data(stream.id(), "hello ", false);
    client.send_stream_data(stream.id(), "quic\n", true);

    EXPECT_TRUE(pump_until(
        [&] {
            auto *response = client.stream_session(stream.id());
            return response && response->last_message == "hello quic";
        },
        std::chrono::seconds(5)));

    auto *server_session = server.stream_session(stream.id());
    auto *client_session = client.stream_session(stream.id());
    ASSERT_NE(server_session, nullptr);
    ASSERT_NE(client_session, nullptr);
    EXPECT_EQ(server_session->messages, 1u);
    EXPECT_EQ(client_session->last_message, "hello quic");

    client.close();
    server.close();
    async::listener::current.clear();
}

/**
 * @test Many command streams multiplex over one QUIC connection
 * @brief Opens 16 bidirectional streams on one connection, each sending `stream-i\n`; pumps until every
 *        client session echoes back `stream-i`, then asserts both endpoints registered exactly 16 sessions.
 */
TEST(TextSessionQuic, ManyCommandStreamsOverOneConnection) {
    ASSERT_TRUE(qb::io::test::require_ssl_files()) << "QUIC build ships its TLS certs; their absence is a packaging regression";

    async::init();

    ProtocolQuicServer<EchoTextQuicSession> server;
    TextProtocolQuicClient                  client;
    connect_local_quic_pair(server, client);

    constexpr std::size_t      stream_count = 16;
    std::vector<std::uint64_t> stream_ids;
    stream_ids.reserve(stream_count);
    for (std::size_t i = 0; i < stream_count; ++i) {
        auto stream = client.open_bidirectional_stream();
        stream_ids.push_back(stream.id());
        client.send_stream_data(stream.id(), std::string{"stream-"} + std::to_string(i) + "\n", true);
    }

    EXPECT_TRUE(pump_until(
        [&] {
            for (std::size_t i = 0; i < stream_ids.size(); ++i) {
                auto *session = client.stream_session(stream_ids[i]);
                if (!session || session->last_message != std::string{"stream-"} + std::to_string(i))
                    return false;
            }
            return true;
        },
        std::chrono::seconds(8)));

    EXPECT_EQ(server.session_count(), stream_count);
    EXPECT_EQ(client.session_count(), stream_count);

    client.close();
    server.close();
    async::listener::current.clear();
}

#endif // QB_HAS_QUIC
