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
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>
#include <qb/io/protocol/text.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/loopback_fixture.h"
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

constexpr std::size_t NB_ITERATION     = 64; // correctness count; throughput is owned by the benchmark
constexpr char        STRING_MESSAGE[] = "Here is my content test";
constexpr std::size_t MESSAGE_LEN      = sizeof(STRING_MESSAGE) - 1;

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
        server.transport().init(ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
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
        server.transport().init(ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
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

    EXPECT_TRUE(pump_until([&] { return Policy::Counters::server.load() == NB_ITERATION && Policy::Counters::client.load() == NB_ITERATION; },
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

        EXPECT_TRUE(
            pump_until([&] { return bin_server.load() >= BIN_ITERATIONS && bin_client.load() >= BIN_ITERATIONS; }, std::chrono::seconds(10)));
    });

    EXPECT_TRUE(
        pump_until([&] { return bin_server.load() >= BIN_ITERATIONS && bin_client.load() >= BIN_ITERATIONS; }, std::chrono::seconds(10)));
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

// Read exactly `want` bytes within a deadline; returns bytes read.
//
// The deadline is only a deadline because the read is non-blocking. This helper used
// to poll a socket the caller had flipped to BLOCKING, so `steady_clock::now() < deadline`
// was re-checked only between reads it could never return from: a server that stopped
// sending mid-frame parked the worker thread in the kernel for good, and the `worker.join()`
// at the end of the test then hung the binary — turning "the protocol switch did not echo
// the frame back" into a ctest timeout that reads as runner flake.
std::size_t
read_exact(qb::io::tcp::socket &sock, char *buf, std::size_t want) {
    return qb::io::test::read_exact_within(sock, buf, want, std::chrono::seconds(5));
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

    std::atomic<bool>     worker_ok{false};
    constexpr char        payload[]   = "binary_data!";
    constexpr std::size_t payload_len = sizeof(payload) - 1;

    std::thread worker([&] {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);

        // text round-trip. The server echoes `msg.text << Protocol::end`, i.e.
        // "hello" + '\n' (6 bytes) — read the FULL echoed line including the
        // terminator so no stray '\n' is left to corrupt the later binary frame.
        ASSERT_GT(sock.write("hello\n", 6), 0);
        char echo[6]{};
        ASSERT_EQ(read_exact(sock, echo, 6), 6u);
        EXPECT_EQ(std::string_view(echo, 6), "hello\n");

        // trigger the protocol switch and wait for it (no wall-clock guess)
        ASSERT_GT(sock.write("SWITCH\n", 7), 0);
        EXPECT_TRUE(pump_until([&] { return switch_done.load(); }, std::chrono::seconds(5))) << "server never switched to binary16";

        // one binary frame: 16-bit length + payload
        const std::uint16_t len = htons(static_cast<std::uint16_t>(payload_len));
        ASSERT_GT(sock.write(reinterpret_cast<const char *>(&len), sizeof(len)), 0);
        ASSERT_GT(sock.write(payload, payload_len), 0);

        // read back the full echoed binary frame (length prefix + payload), unconditionally
        char       reply[2 + payload_len]{};
        const auto got = read_exact(sock, reply, sizeof(reply));
        ASSERT_EQ(got, sizeof(reply)) << "short read of the echoed binary frame";
        const std::uint16_t got_len = ntohs(*reinterpret_cast<std::uint16_t *>(reply));
        ASSERT_EQ(got_len, payload_len);
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
        EXPECT_TRUE(pump_until([&] { return udp_client.load() >= 1; }, std::chrono::seconds(5))) << "client never received any UDP echo";
    });

    EXPECT_TRUE(pump_until([&] { return udp_server.load() >= 1; }, std::chrono::seconds(5))) << "server never received any UDP datagram";
    worker.join();

    EXPECT_GE(udp_server.load(), 1u);
    EXPECT_GE(udp_client.load(), 1u);
    async::listener::current.clear();
}

namespace {

// What the frame server below saw, in order: the frames, the port each one's reply would go to, and the bytes each
// `pending_read` reported (an incomplete trailing frame). Touched by the test thread only (it pumps the loop).
struct udp_framing_seen {
    std::vector<std::string>   texts;
    std::vector<std::uint16_t> reply_ports;
    std::vector<std::size_t>   pending;
};
udp_framing_seen udp_framing;

class UdpFrameServer : public use<UdpFrameServer>::udp::server {
public:
    using Protocol = qb::protocol::text::command<UdpFrameServer>;
    void
    on(Protocol::message &&msg) {
        udp_framing.texts.emplace_back(msg.text);
        udp_framing.reply_ports.push_back(getSource().port());
    }
    void
    on(async::event::pending_read &&evt) {
        udp_framing.pending.push_back(evt.bytes);
    }
};

} // namespace

/**
 * @test A datagram carries whole frames: an incomplete tail is reported, then dropped (Huly QB-303)
 * @brief The async UDP component framed successive datagrams as ONE byte stream: peer A's incomplete `abc` stayed
 *        in the input and became the head of peer B's `def\n` -- one frame `abcdef`, answered to B. Now each datagram
 *        is framed alone: what the protocol leaves is reported as `pending_read` and dropped with the protocol's
 *        partial state. Controls: several whole frames in one datagram, and a dropped tail that does not prefix the
 *        next datagram's frame.
 */
TEST(TextSessionUdp, ADatagramCarriesWholeFramesAndAnIncompleteTailIsDropped) {
    async::init();
    udp_framing = {};

    UdpFrameServer server;
    ASSERT_EQ(server.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto to = endpoint().as_in("127.0.0.1", server.transport().local_endpoint().port());
    server.start();

    qb::io::udp::socket peer_a;
    qb::io::udp::socket peer_b;
    ASSERT_TRUE(peer_a.init());
    ASSERT_TRUE(peer_b.init());
    ASSERT_EQ(peer_a.bind_v4(0, "127.0.0.1"), 0);
    ASSERT_EQ(peer_b.bind_v4(0, "127.0.0.1"), 0);

    // Peer A: an incomplete frame, alone in its datagram. Reported, then dropped.
    ASSERT_EQ(peer_a.write("abc", 3, to), 3);
    ASSERT_TRUE(pump_until([] { return udp_framing.pending.size() == 1; }, std::chrono::seconds(2))) << "A's datagram never read";
    EXPECT_EQ(udp_framing.pending[0], 3u);
    EXPECT_TRUE(udp_framing.texts.empty());

    // Peer B: a whole frame. It arrives alone -- not glued to A's tail -- and its reply goes to B.
    ASSERT_EQ(peer_b.write("def\n", 4, to), 4);
    ASSERT_TRUE(pump_until([] { return udp_framing.texts.size() == 1; }, std::chrono::seconds(2))) << "B's frame never delivered";
    EXPECT_EQ(udp_framing.texts[0], "def") << "A's incomplete frame became the head of B's";
    EXPECT_EQ(udp_framing.reply_ports[0], peer_b.local_endpoint().port());

    // Several whole frames then a tail in ONE datagram: the frames are delivered, the tail reported and dropped,
    // and it does not prefix the next datagram's frame.
    ASSERT_EQ(peer_b.write("x\ny\nzz", 6, to), 6);
    ASSERT_TRUE(pump_until([] { return udp_framing.texts.size() == 3 && udp_framing.pending.size() == 2; }, std::chrono::seconds(2)));
    EXPECT_EQ(udp_framing.texts[1], "x");
    EXPECT_EQ(udp_framing.texts[2], "y");
    EXPECT_EQ(udp_framing.pending[1], 2u);
    ASSERT_EQ(peer_a.write("w\n", 2, to), 2);
    ASSERT_TRUE(pump_until([] { return udp_framing.texts.size() == 4; }, std::chrono::seconds(2)));
    EXPECT_EQ(udp_framing.texts[3], "w") << "the dropped tail 'zz' prefixed the next datagram's frame";
    EXPECT_EQ(udp_framing.reply_ports[3], peer_a.local_endpoint().port());
    async::listener::current.clear();
}

// =============================================================================
// LIVE QUIC LOOPBACK (real UDP + local TLS; QB_HAS_QUIC only, certs REQUIRED)
// =============================================================================

#ifdef QB_HAS_QUIC

namespace {

class EchoTextQuicSession : public use<EchoTextQuicSession>::quic::session {
public:
    using Protocol       = qb::protocol::text::command<EchoTextQuicSession>;
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

namespace {

// The frames a pausing UDP component saw, and the bytes each `pending_read` reported. Touched by the test thread only.
struct udp_pause_seen {
    std::vector<std::string> texts;
    std::vector<std::size_t> pending;
    std::size_t              pause_at          = 0;
    bool                     resume_at_pending = false; ///< on(pending_read) resumes a pause the dispatch made
};
udp_pause_seen udp_pause;

/// The handler of frame `pause_at` pauses the server's reads.
class UdpPausingServer : public use<UdpPausingServer>::udp::server {
public:
    using Protocol = qb::protocol::text::command<UdpPausingServer>;
    void
    on(Protocol::message &&msg) {
        udp_pause.texts.emplace_back(msg.text);
        if (udp_pause.texts.size() == udp_pause.pause_at)
            this->pause_reading();
    }
    void
    on(async::event::pending_read &&evt) {
        udp_pause.pending.push_back(evt.bytes);
        if (udp_pause.resume_at_pending && this->reading_paused())
            this->resume_reading();
    }
};

/// Its twin on `input<>` alone: a component that only reads datagrams, as udp::server composes io<> with the transport.
class UdpPausingInput
    : public qb::io::async::input<UdpPausingInput>
    , public qb::io::transport::udp {
public:
    using Protocol                         = qb::protocol::text::command<UdpPausingInput>;
    constexpr static const bool has_server = false;
    UdpPausingInput() {
        this->template switch_protocol<Protocol>(*this);
    }
    ~UdpPausingInput() {
        this->_async_event.stop(); // before the transport closes its descriptor, as udp::server does
    }
    void
    on(Protocol::message &&msg) {
        udp_pause.texts.emplace_back(msg.text);
        if (udp_pause.texts.size() == udp_pause.pause_at)
            this->pause_reading();
    }
    void
    on(async::event::pending_read &&evt) {
        udp_pause.pending.push_back(evt.bytes);
        if (udp_pause.resume_at_pending && this->reading_paused())
            this->resume_reading();
    }
};

/// Three whole frames and a tail in ONE datagram, the handler of the first pausing: the other two are framed once
/// reading resumes, and the tail reported and dropped then -- not glued to the next datagram's frame.
template <typename Component>
void
expect_a_datagrams_frames_to_survive_a_pause(bool resume_at_pending = false) {
    async::init();
    udp_pause                   = {};
    udp_pause.pause_at          = 1;
    udp_pause.resume_at_pending = resume_at_pending;
    {
        Component component;
        ASSERT_EQ(component.transport().bind_v4(0, "127.0.0.1"), 0);
        const auto to = endpoint().as_in("127.0.0.1", component.transport().local_endpoint().port());
        component.start();
        qb::io::udp::socket peer;
        ASSERT_TRUE(peer.init());

        ASSERT_EQ(peer.write("x\ny\nz\nzz", 8, to), 8);
        ASSERT_TRUE(pump_until([] { return !udp_pause.texts.empty(); }, std::chrono::seconds(2))) << "the datagram was never read";
        (void) pump_until([] { return false; }, std::chrono::milliseconds(50));
        if (!resume_at_pending) {
            EXPECT_EQ(udp_pause.texts, (std::vector<std::string>{"x"})) << "a paused component framed on";
            EXPECT_TRUE(component.reading_paused());
            component.resume_reading();
        }
        ASSERT_TRUE(pump_until([] { return udp_pause.texts.size() >= 3 && udp_pause.pending.size() >= 2; }, std::chrono::seconds(2)))
            << "the datagram's frames were lost across the pause";
        EXPECT_EQ(udp_pause.texts, (std::vector<std::string>{"x", "y", "z"}));
        EXPECT_EQ(udp_pause.pending.back(), 2u) << "the tail reported was not the datagram's incomplete frame";
        ASSERT_EQ(peer.write("w\n", 2, to), 2);
        ASSERT_TRUE(pump_until([] { return udp_pause.texts.size() == 4; }, std::chrono::seconds(2)));
        EXPECT_EQ(udp_pause.texts.back(), "w") << "the dropped tail prefixed the next datagram's frame";
    }
    async::listener::current.clear();
}

/// The next datagram arrives while paused: the resume frames the rest of the interrupted one and drops its tail first,
/// then reads it -- read first, it would have been glued to that tail.
template <typename Component>
void
expect_the_next_datagram_to_wait_for_the_rest() {
    async::init();
    udp_pause          = {};
    udp_pause.pause_at = 1;
    {
        Component component;
        ASSERT_EQ(component.transport().bind_v4(0, "127.0.0.1"), 0);
        const auto to = endpoint().as_in("127.0.0.1", component.transport().local_endpoint().port());
        component.start();
        qb::io::udp::socket peer;
        ASSERT_TRUE(peer.init());

        ASSERT_EQ(peer.write("x\ny\nz\nzz", 8, to), 8);
        ASSERT_TRUE(pump_until([] { return !udp_pause.texts.empty(); }, std::chrono::seconds(2))) << "the datagram was never read";
        ASSERT_EQ(peer.write("w\n", 2, to), 2);
        (void) pump_until([] { return false; }, std::chrono::milliseconds(50));
        EXPECT_EQ(udp_pause.texts, (std::vector<std::string>{"x"})) << "a paused component framed on";
        component.resume_reading();
        ASSERT_TRUE(pump_until([] { return udp_pause.texts.size() >= 4; }, std::chrono::seconds(2))) << "the frames were lost";
        EXPECT_EQ(udp_pause.texts, (std::vector<std::string>{"x", "y", "z", "w"})) << "the next datagram was read before the rest";
    }
    async::listener::current.clear();
}

/// A component that reads a TCP stream on input<> alone: the handler of its first message pauses its reads.
class TcpPausingInput
    : public qb::io::async::input<TcpPausingInput>
    , public qb::io::transport::tcp {
public:
    using Protocol                         = qb::protocol::text::command<TcpPausingInput>;
    constexpr static const bool has_server = false;
    std::vector<std::string>    texts;
    int                         disconnected = 0;
    int                         error        = 0; ///< the system error its disconnection reported
    TcpPausingInput() {
        this->template switch_protocol<Protocol>(*this);
    }
    ~TcpPausingInput() {
        this->_async_event.stop();
    }
    void
    on(Protocol::message &&msg) {
        texts.emplace_back(msg.text);
        if (texts.size() == 1u)
            this->pause_reading();
    }
    void
    on(async::event::disconnected &&event) {
        ++disconnected;
        error = event.error_code.value();
    }
};

/// An input<> stream component, paused by its first message with the second whole in its input; `then` acts on the
/// peer and the component, then reading resumes: the second is framed whatever the resume's read meets.
template <typename Then>
void
expect_an_inputs_held_message_framed_at_its_resume(Then &&then, bool reset_expected = false) {
    async::init();
    {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), SocketStatus::Done);
        TcpPausingInput     component;
        qb::io::tcp::socket peer;
        ASSERT_EQ(component.transport().connect_v4("127.0.0.1", listener.local_endpoint().port()), SocketStatus::Done);
        ASSERT_EQ(listener.accept(peer), SocketStatus::Done);
        component.start();
        ASSERT_EQ(peer.write("first\nsecond\n", 13), 13);
        ASSERT_TRUE(pump_until([&] { return !component.texts.empty(); }, 2s)) << "the first message never came";
        (void) pump_until([] { return false; }, 50ms);
        ASSERT_EQ(component.texts, (std::vector<std::string>{"first"})) << "a paused input dispatched what its input held";
        then(peer, component);
        component.resume_reading();
        EXPECT_TRUE(pump_until([&] { return component.texts.size() >= 2u; }, 2s)) << "what the input held was not framed at the resume";
        EXPECT_EQ(component.texts, (std::vector<std::string>{"first", "second"}));
        (void) pump_until([] { return false; }, 50ms);
        if (component.disconnected != 0 && !peer.is_open()) // the peer ended the connection: what its end reported
            EXPECT_TRUE(component.error != 0 || !reset_expected) << "a reset was reported as a clean close";
        component.stop();
    }
    async::listener::current.clear();
}

} // namespace

TEST(TextSessionUdp, ADatagramArrivingWhilePausedWaitsForTheRestOfTheOneInterrupted) {
    expect_the_next_datagram_to_wait_for_the_rest<UdpPausingServer>();
}

TEST(TextSessionUdp, AnInputsDatagramArrivingWhilePausedWaitsForTheRestOfTheOneInterrupted) {
    expect_the_next_datagram_to_wait_for_the_rest<UdpPausingInput>();
}

// The resume's read finds nothing, the peer silent: an input frames what the pause left all the same.
TEST(TextSessionPausedInput, AWholeMessageHeldIsFramedThePeerSilent) {
    expect_an_inputs_held_message_framed_at_its_resume([](qb::io::tcp::socket &, TcpPausingInput &) {});
}

// The resume's read meets the peer's close: an input frames what the pause left first, then sees the close.
TEST(TextSessionPausedInput, AWholeMessageHeldIsFramedBeforeThePeersClose) {
    bool seen_before = true;
    expect_an_inputs_held_message_framed_at_its_resume([&](qb::io::tcp::socket &peer, TcpPausingInput &component) {
        peer.close();
        (void) pump_until([] { return false; }, 50ms);
        seen_before = component.disconnected != 0;
    });
    EXPECT_FALSE(seen_before) << "a paused input saw its peer's close";
}

// The resume's read meets the peer's reset: an input frames what the pause left first, then reports the reset with its
// error -- the read after it would meet an end of stream.
TEST(TextSessionPausedInput, AWholeMessageHeldIsFramedThenThePeersResetReported) {
    TcpPausingInput *seen = nullptr;
    expect_an_inputs_held_message_framed_at_its_resume(
        [&](qb::io::tcp::socket &peer, TcpPausingInput &component) {
            ::linger zero{1, 0};
            EXPECT_EQ(peer.set_optval(SOL_SOCKET, SO_LINGER, zero), 0);
            peer.close();
            (void) pump_until([] { return false; }, 50ms);
            seen = &component;
        },
        true);
    ASSERT_NE(seen, nullptr);
}

// The resume's read is refused by a read cap lowered during the pause: an input frames what the pause left first.
TEST(TextSessionPausedInput, AWholeMessageHeldPastTheReadCapIsFramed) {
    int disconnected = -1;
    expect_an_inputs_held_message_framed_at_its_resume([&](qb::io::tcp::socket &, TcpPausingInput &component) {
        component.set_max_read_buffer_size(4u);
        disconnected = component.disconnected;
    });
    EXPECT_EQ(disconnected, 0);
}

/**
 * @test A datagram's frames survive a pause made from the handler of the first: framed once reading resumes, and the
 *       incomplete tail still reported and dropped then (pause_reading(), Huly QB-303)
 * @brief The per-datagram drop of what the protocol left ran under the pause too, and discarded the rest of the
 *        datagram's whole frames as its tail.
 */
TEST(TextSessionUdp, ADatagramsFramesSurviveAPauseMadeFromTheHandlerOfTheFirst) {
    expect_a_datagrams_frames_to_survive_a_pause<UdpPausingServer>();
}

TEST(TextSessionUdp, AnInputsDatagramFramesSurviveAPauseMadeFromTheHandlerOfTheFirst) {
    expect_a_datagrams_frames_to_survive_a_pause<UdpPausingInput>();
}

// ...and resumed from on(pending_read) of the dispatch the pause stopped: the datagram's frames are kept all the same.
TEST(TextSessionUdp, ADatagramsFramesSurviveAPauseResumedFromItsPendingRead) {
    expect_a_datagrams_frames_to_survive_a_pause<UdpPausingServer>(true);
}

TEST(TextSessionUdp, AnInputsDatagramFramesSurviveAPauseResumedFromItsPendingRead) {
    expect_a_datagrams_frames_to_survive_a_pause<UdpPausingInput>(true);
}

// =============================================================================
// PAUSED READS OVER TCP AND TLS (Huly QB-86): what a resume frames, and what a paused session learns of its peer
// =============================================================================

namespace {

// What the pausing server sessions saw. Touched by the test thread alone: the server runs on it.
struct tcp_pause_seen {
    std::vector<std::string> texts;
    std::vector<std::size_t> pending; ///< what each `pending_read` reported
    int                      disconnected      = 0;
    int                      error             = 0; ///< the system error the last disconnection reported
    int                      timeouts          = 0;
    bool                     paused_at_timeout = false;
    bool                     pause_every       = false; ///< every message's handler pauses, not only the first's
    std::size_t              stop_at           = 0;     ///< the answer to this message (1-based) is followed by a stop()
    std::size_t              close_at          = 0;     ///< ...or by a close_after_deliver()
};
tcp_pause_seen tcp_pause;

/// The handler of the first message pauses the session's reads.
template <typename Session>
void
seen_text(Session &session, std::string_view text) {
    tcp_pause.texts.emplace_back(text);
    if (tcp_pause.texts.size() == 1u || tcp_pause.pause_every)
        session.pause_reading();
}

/// After a message's answer: the stop() or close_after_deliver() a case asks for.
template <typename Session>
void
answered(Session &session) {
    if (tcp_pause.texts.size() == tcp_pause.stop_at)
        session.stop();
    if (tcp_pause.texts.size() == tcp_pause.close_at)
        session.close_after_deliver();
}

class TcpPauseServer;
class TcpPauseSession
    : public use<TcpPauseSession>::tcp::client<TcpPauseServer>
    , public async::with_timeout<TcpPauseSession> {
public:
    using Protocol = qb::protocol::text::command<TcpPauseSession>;
    explicit TcpPauseSession(IOServer &server)
        : client(server)
        , with_timeout(0s) {}
    void
    on(Protocol::message &&msg) {
        seen_text(*this, msg.text);
        *this << msg.text << Protocol::end; // the answer: read by the peers that read
        answered(*this);
    }
    void
    on(async::event::pending_read &&evt) {
        tcp_pause.pending.push_back(evt.bytes);
    }
    void
    on(async::event::timer &) {
        ++tcp_pause.timeouts;
        tcp_pause.paused_at_timeout = reading_paused();
        disconnect();
    }
    void
    on(async::event::disconnected &&event) {
        ++tcp_pause.disconnected;
        tcp_pause.error = event.error_code.value();
    }
    void
    cap_reads(std::size_t octets) {
        this->set_max_read_buffer_size(octets);
    }
};
class TcpPauseServer : public use<TcpPauseServer>::tcp::server<TcpPauseSession> {};

struct TcpPause {
    using Server = TcpPauseServer;
    using Peer   = qb::io::tcp::socket;
    static bool
    precondition() {
        return true;
    }
    static unsigned short
    listen(Server &server) {
        EXPECT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        return server.transport().local_endpoint().port();
    }
    static bool
    connect(Peer &peer, unsigned short port) {
        return peer.connect_v4("127.0.0.1", port) == SocketStatus::Done;
    }
    /// Its FIN: it sends nothing more, and reads on.
    static bool
    end_sending(Peer &peer) {
        return ::shutdown(peer.native_handle(), QB_SD_SEND) == 0;
    }
    /// What the session sends until its end of stream, or until `span` elapsed.
    static std::string
    read_to_end(Peer &peer, qb::duration span) {
        std::string received;
        char        buffer[256];
        peer.set_nonblocking(true);
        const auto deadline = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = peer.read(buffer, sizeof buffer);
            if (n > 0)
                received.append(buffer, static_cast<std::size_t>(n));
            else if (n == 0 || !qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                break;
            else
                std::this_thread::sleep_for(1ms);
        }
        return received;
    }
};

#ifdef QB_HAS_SSL
class TlsPauseServer;
class TlsPauseSession
    : public use<TlsPauseSession>::tcp::ssl::client<TlsPauseServer>
    , public async::with_timeout<TlsPauseSession> {
public:
    using Protocol = qb::protocol::text::command<TlsPauseSession>;
    explicit TlsPauseSession(IOServer &server)
        : client(server)
        , with_timeout(0s) {}
    void
    on(Protocol::message &&msg) {
        seen_text(*this, msg.text);
        *this << msg.text << Protocol::end; // the answer: read by the peers that read
        answered(*this);
    }
    void
    on(async::event::pending_read &&evt) {
        tcp_pause.pending.push_back(evt.bytes);
    }
    void
    on(async::event::timer &) {
        ++tcp_pause.timeouts;
        tcp_pause.paused_at_timeout = reading_paused();
        disconnect();
    }
    void
    on(async::event::disconnected &&event) {
        ++tcp_pause.disconnected;
        tcp_pause.error = event.error_code.value();
    }
    void
    cap_reads(std::size_t octets) {
        this->set_max_read_buffer_size(octets);
    }
};
class TlsPauseServer : public use<TlsPauseServer>::tcp::ssl::server<TlsPauseSession> {};

struct TlsPause {
    using Server = TlsPauseServer;
    using Peer   = qb::io::tcp::ssl::socket;
    static bool
    precondition() {
        return qb::io::test::require_ssl_files();
    }
    static unsigned short
    listen(Server &server) {
        server.transport().init(ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
        EXPECT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        return server.transport().local_endpoint().port();
    }
    static bool
    connect(Peer &peer, unsigned short port) {
        peer.set_insecure(); // the local self-signed certificate
        return peer.connect_v4("127.0.0.1", port) == 0;
    }
    /// Its close_notify ALONE: the TCP connection stays open, and it reads on -- a TLS half-close.
    static bool
    end_sending(Peer &peer) {
        return peer.close_notify();
    }
    /// What the session sends until its close_notify or end of stream, or until `span` elapsed.
    static std::string
    read_to_end(Peer &peer, qb::duration span) {
        std::string received;
        char        buffer[256];
        peer.set_nonblocking(true);
        const auto deadline = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < deadline) {
            const int n = peer.read(buffer, sizeof buffer);
            if (n > 0)
                received.append(buffer, static_cast<std::size_t>(n));
            else if (n < 0 && !qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                break; // the session's close_notify (no error), or its end
            else
                std::this_thread::sleep_for(1ms); // nothing yet
        }
        return received;
    }
};
#endif

/// Runs the loop for `span`, whatever happens.
void
pause_pump_for(qb::duration span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until)
        async::run(EVRUN_NOWAIT);
}

/// A server on the test thread, and a peer on its own thread that sends one text in ONE write, then waits for the test
/// to tell it how to end: close (its FIN, and over TLS its close_notify first), reset (a zero linger, no close_notify),
/// or half (its sending side alone -- over TLS its close_notify alone, the TCP connection open --: it reads what the
/// session sends until the session's end, then waits for the test's release before it closes).
template <typename Kind>
class PausedReads : public ::testing::Test {
protected:
    enum : int { wait = 0, close = 1, reset = 2, half = 3 };
    typename Kind::Server server;
    std::atomic<int>      peer_end{wait};
    std::atomic<bool>     peer_sent{false};
    std::atomic<bool>     peer_done{false};
    std::atomic<bool>     peer_more{false};    ///< the peer sends `more` once, while it waits for its end
    std::atomic<bool>     peer_halved{false};  ///< half: the peer's sending side has ended
    std::atomic<bool>     peer_read{false};    ///< half: the peer has read until the session's end (`received`)
    std::atomic<bool>     peer_release{false}; ///< half: the peer may close
    std::string           more;
    std::string           received; ///< half: what the peer read, written by its thread before `peer_read`
    std::thread           peer;

    void
    SetUp() override {
        async::init();
        tcp_pause = {};
    }

    void
    TearDown() override {
        if (peer_end.load() == wait)
            peer_end.store(close);
        peer_release.store(true);
        if (peer.joinable()) {
            EXPECT_TRUE(pump_until([&] { return peer_done.load(); }, 5s));
            peer.join();
        }
        async::listener::current.clear();
    }

    void
    start(std::string text) {
        const unsigned short port = Kind::listen(server);
        server.start();
        peer = std::thread([this, port, text] {
            typename Kind::Peer sock;
            if (Kind::connect(sock, port)) {
                EXPECT_EQ(sock.write(text.data(), text.size()), static_cast<int>(text.size()));
                peer_sent.store(true);
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                while (peer_end.load() == wait && std::chrono::steady_clock::now() < deadline) {
                    if (peer_more.exchange(false))
                        EXPECT_EQ(sock.write(more.data(), more.size()), static_cast<int>(more.size()));
                    std::this_thread::sleep_for(1ms);
                }
                if (peer_end.load() == reset) {
                    ::linger zero{1, 0};
                    EXPECT_EQ(sock.set_optval(SOL_SOCKET, SO_LINGER, zero), 0);
                    sock.close(); // a reset: no close_notify first over TLS
                } else if (peer_end.load() == half) {
                    EXPECT_TRUE(Kind::end_sending(sock));
                    peer_halved.store(true);
                    received = Kind::read_to_end(sock, 8s);
                    peer_read.store(true);
                    const auto hold = std::chrono::steady_clock::now() + 40s;
                    while (!peer_release.load() && std::chrono::steady_clock::now() < hold)
                        std::this_thread::sleep_for(1ms);
                    sock.close();
                } else
                    sock.disconnect();
            }
            peer_sent.store(true);
            peer_done.store(true);
        });
        ASSERT_TRUE(pump_until([&] { return !tcp_pause.texts.empty() && !server.sessions().empty(); }, 5s)) << "the first message never came";
        pause_pump_for(50ms);
        ASSERT_EQ(server.sessions().size(), 1u);
        ASSERT_TRUE(session().reading_paused()) << "the first message's handler did not pause the session";
    }

    auto &
    session() {
        return *server.sessions().begin()->second;
    }

    /// The peer ends as asked, and the paused session is left to the loop for a while.
    void
    end_peer(int how) {
        peer_end.store(how);
        ASSERT_TRUE(pump_until([&] { return peer_done.load(); }, 5s));
        pause_pump_for(100ms);
    }

    /// The peer ends its sending side alone, and the paused session is left to the loop for a while.
    void
    half_end_peer() {
        peer_end.store(half);
        ASSERT_TRUE(pump_until([&] { return peer_halved.load(); }, 5s));
        pause_pump_for(100ms);
        ASSERT_EQ(tcp_pause.disconnected, 0) << "a paused session saw its peer's end";
    }

    /// The session ends once, within `span`: the read that met the end is not reported twice.
    void
    expect_one_end_within(std::chrono::milliseconds span) {
        EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected != 0; }, span)) << "the session never met its peer's end";
        pause_pump_for(100ms);
        EXPECT_EQ(tcp_pause.disconnected, 1) << "the end was reported more than once";
        EXPECT_EQ(tcp_pause.error, 0) << "the peer's end was reported as an error";
    }

    /// What the peer read before the session's end: everything the session answered.
    void
    expect_the_peer_to_have_read(std::string const &answers) {
        ASSERT_TRUE(pump_until([&] { return peer_read.load(); }, 10s));
        EXPECT_EQ(received, answers) << "an answer never reached the peer";
    }
};

using PausedReadTransports = ::testing::Types<TcpPause
#ifdef QB_HAS_SSL
                                              ,
                                              TlsPause
#endif
                                              >;
TYPED_TEST_SUITE(PausedReads, PausedReadTransports);

} // namespace

// A whole message the input already holds, nothing more on the wire: the resume frames it -- a dispatch fed for it alone
// would end at its read, which finds nothing, before its framing.
TYPED_TEST(PausedReads, AWholeMessageReadBeforeThePauseIsFramedWhenReadingResumes) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\nsecond\n");
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first"})) << "a paused session dispatched what its input held";
    ASSERT_FALSE(tcp_pause.pending.empty()) << "the case proves nothing: the second message was not read with the first";
    EXPECT_EQ(tcp_pause.pending.back(), std::string_view("second\n").size());

    this->session().resume_reading(); // from the test thread: outside a dispatch
    ASSERT_TRUE(pump_until([] { return tcp_pause.texts.size() >= 2u; }, 2s))
        << "what the input held was not framed once reading resumed, the peer sending nothing more";
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"}));
    EXPECT_FALSE(this->session().reading_paused());
    EXPECT_EQ(tcp_pause.disconnected, 0);
}

// The peer sends while the session is paused: nothing of it is read, and the resume reads it -- on epoll, the
// readiness the paused descriptor raised made libev delete it from the epoll set, and the resume registers it again.
TYPED_TEST(PausedReads, WhatThePeerSendsWhilePausedIsReadOnceReadingResumes) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\n");
    this->more = "second\n";
    this->peer_more.store(true);
    ASSERT_TRUE(pump_until([&] { return !this->peer_more.load(); }, 2s));
    pause_pump_for(100ms);
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first"})) << "a paused session read what its peer sent";
    EXPECT_EQ(tcp_pause.disconnected, 0);

    this->session().resume_reading();
    ASSERT_TRUE(pump_until([] { return tcp_pause.texts.size() >= 2u; }, 2s)) << "what the peer sent during the pause was never read";
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"}));
}

// A whole message the input holds when the peer closes during the pause: the resume frames it, then sees the close --
// the read that meets the close frames what the pause left first.
TYPED_TEST(PausedReads, AWholeMessageHeldWhenThePeerClosesIsFramedBeforeTheClose) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\nsecond\n");
    this->end_peer(TestFixture::close);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a paused session saw its peer's close";
    this->session().resume_reading();
    EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected == 1; }, 2s)) << "the resumed session never saw its peer's close";
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"})) << "the close was seen before what the input held was framed";
}

// What the pause left fills the input past a read cap lowered meanwhile: the resume frames it -- the read the cap
// refuses frames what the pause left first -- and the session goes on.
TYPED_TEST(PausedReads, AWholeMessageHeldPastTheReadCapIsFramedWhenReadingResumes) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\nsecond\n");
    this->session().cap_reads(4u); // what the input holds, "second\n", is past it
    this->session().resume_reading();
    ASSERT_TRUE(pump_until([] { return tcp_pause.texts.size() >= 2u || tcp_pause.disconnected != 0; }, 2s));
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"})) << "the read the cap refused ended the session first";
    EXPECT_EQ(tcp_pause.disconnected, 0);
}

// A whole message the input holds when the peer resets during the pause: the resume frames it, then reports the reset
// with its error -- the read that met it meets an end of stream next time -- as a session never paused reports it.
TYPED_TEST(PausedReads, AWholeMessageHeldWhenThePeerResetsIsFramedAndTheResetReported) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\nsecond\n");
    this->end_peer(TestFixture::reset);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a paused session saw its peer's reset";
    this->session().resume_reading();
    EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected == 1; }, 2s)) << "the resumed session never saw its peer's reset";
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"})) << "the reset was seen before what the input held was framed";
    EXPECT_NE(tcp_pause.error, 0) << "the reset was reported as a clean close";
}

// The peer closes while the session is paused: the session hears nothing of it -- its reads are off on every backend --
// until reading resumes, which reads the close and ends the session.
TYPED_TEST(PausedReads, APausedSessionSeesItsPeersCloseOnceReadingResumes) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\n");
    this->end_peer(TestFixture::close);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a paused session saw its peer's close";
    ASSERT_EQ(this->server.sessions().size(), 1u);
    EXPECT_TRUE(this->session().reading_paused());

    this->session().resume_reading();
    EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected == 1; }, 2s)) << "the resumed session never saw its peer's close";
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first"}));
}

// The peer resets the connection while the session is paused: the session hears nothing of it until it writes, and
// that write ends it.
TYPED_TEST(PausedReads, APausedSessionSeesItsPeersResetAtItsNextWrite) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\n");
    this->end_peer(TestFixture::reset);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a paused session saw its peer's reset";
    ASSERT_EQ(this->server.sessions().size(), 1u);

    this->session() << "late\n";
    EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected == 1; }, 2s)) << "the write of a paused session never met its peer's reset";
}

// The peer ends its sending side alone during the pause and reads on -- over TLS its close_notify alone, the TCP
// connection kept open, as a TLS half-close leaves it: the resume frames what the input held, writes its answer, and
// sees the end once. Over TLS the read that meets the close_notify takes it: the socket says nothing more, and the end
// is read again by a dispatch of its own (Huly QB-86).
TYPED_TEST(PausedReads, AWholeMessageHeldWhenThePeerEndsItsSendingSideIsAnsweredAndTheEndSeenOnce) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\nsecond\n");
    this->half_end_peer();
    this->session().resume_reading();
    this->expect_one_end_within(5s);
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"}));
    this->expect_the_peer_to_have_read("first\nsecond\n");
}

// ...the handler of what the input held pauses again: the end waits for the next resume, which sees it once.
TYPED_TEST(PausedReads, AHandlerThatPausesAgainAtThePeersEndSeesItOnceReadingResumes) {
    ASSERT_TRUE(TypeParam::precondition());
    tcp_pause.pause_every = true;
    this->start("first\nsecond\n");
    this->half_end_peer();
    this->session().resume_reading();
    ASSERT_TRUE(pump_until([] { return tcp_pause.texts.size() == 2u; }, 2s)) << "what the input held was never framed";
    pause_pump_for(100ms);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a session paused again saw its peer's end";
    ASSERT_EQ(this->server.sessions().size(), 1u) << "the session paused again ended";
    ASSERT_TRUE(this->session().reading_paused());
    this->session().resume_reading();
    this->expect_one_end_within(5s);
    this->expect_the_peer_to_have_read("first\nsecond\n");
}

// ...the handler of what the input held stops the session: its next start() sees the end once.
TYPED_TEST(PausedReads, AHandlerThatStopsTheSessionAtThePeersEndSeesItAtTheNextStart) {
    ASSERT_TRUE(TypeParam::precondition());
    tcp_pause.stop_at = 2u;
    this->start("first\nsecond\n");
    this->half_end_peer();
    this->session().resume_reading();
    ASSERT_TRUE(pump_until([] { return tcp_pause.texts.size() == 2u; }, 2s)) << "what the input held was never framed";
    pause_pump_for(100ms);
    EXPECT_EQ(tcp_pause.disconnected, 0) << "a stopped session saw its peer's end";
    ASSERT_EQ(this->server.sessions().size(), 1u) << "the stopped session ended";
    this->session().start();
    this->expect_one_end_within(5s);
    this->expect_the_peer_to_have_read("first\nsecond\n");
}

// ...the handler of what the input held closes after delivering its answer: the end the read met ends the linger at once
// -- its bound set long, so a linger left to wait for an end already read could not pass -- and once.
TYPED_TEST(PausedReads, ACloseAfterDeliverAtThePeersEndEndsTheSessionOnceWithoutItsLinger) {
    ASSERT_TRUE(TypeParam::precondition());
    tcp_pause.close_at = 2u;
    this->start("first\nsecond\n");
    this->session().set_linger(30s);
    this->half_end_peer();
    this->session().resume_reading();
    this->expect_one_end_within(5s);
    EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"}));
    this->expect_the_peer_to_have_read("first\nsecond\n");
}

// A peer that sends nothing more while the session is paused: the session's own timeout still runs, and ends it.
TYPED_TEST(PausedReads, APausedSessionsOwnTimeoutStillBoundsItsWait) {
    ASSERT_TRUE(TypeParam::precondition());
    this->start("first\n");
    this->session().setTimeout(200ms);
    EXPECT_TRUE(pump_until([] { return tcp_pause.timeouts == 1 && tcp_pause.disconnected == 1; }, 3s))
        << "the timeout of a paused session never fired";
    EXPECT_TRUE(tcp_pause.paused_at_timeout) << "the case proves nothing: the session was not paused when its timeout fired";
}

namespace {

class PausingAcceptor : public use<PausingAcceptor>::tcp::acceptor {
public:
    std::size_t accepted       = 0;
    std::size_t disconnections = 0;
    void
    on(accepted_socket_type &&socket) {
        ++accepted;
        socket.close();
    }
    void
    on(async::event::disconnected &&) {
        ++disconnections;
    }
    [[nodiscard]] bool
    active() const noexcept {
        return this->_async_event.is_active();
    }
};

} // namespace

// An acceptor is an input<>: paused, it accepts nothing; resumed, it accepts the connections that waited. Paused, then
// disposed, its resume does not start it again -- it used to be dispatched on every pass, accepting nothing.
TEST(TextSessionPausedAcceptor, AcceptsNothingPausedAndStaysStoppedOnceDisposed) {
    async::init();
    {
        PausingAcceptor acceptor;
        ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
        const auto port = acceptor.transport().local_endpoint().port();
        acceptor.start();
        qb::io::tcp::socket first, second, third;
        ASSERT_EQ(first.connect_v4("127.0.0.1", port), SocketStatus::Done);
        ASSERT_TRUE(pump_until([&] { return acceptor.accepted == 1u; }, 2s));
        acceptor.pause_reading();
        ASSERT_EQ(second.connect_v4("127.0.0.1", port), SocketStatus::Done);
        pause_pump_for(50ms);
        EXPECT_EQ(acceptor.accepted, 1u) << "a paused acceptor accepted";
        acceptor.resume_reading();
        EXPECT_TRUE(pump_until([&] { return acceptor.accepted == 2u; }, 2s)) << "a resumed acceptor never accepted what waited";

        acceptor.pause_reading();
        acceptor.disconnect();
        ASSERT_TRUE(pump_until([&] { return acceptor.disconnections == 1u; }, 2s));
        ASSERT_EQ(third.connect_v4("127.0.0.1", port), SocketStatus::Done);
        acceptor.resume_reading();
        EXPECT_FALSE(acceptor.active()) << "resume_reading() started a disposed acceptor again";
        pause_pump_for(50ms);
        EXPECT_EQ(acceptor.accepted, 2u) << "a disposed acceptor accepted after its resume";
        EXPECT_EQ(acceptor.disconnections, 1u);
        acceptor.stop();
    }
    async::listener::current.clear();
}

namespace {

/// A component whose message handler pauses its partner's reads.
class CrossPausingClient : public use<CrossPausingClient>::tcp::client<> {
public:
    using Protocol                   = qb::protocol::text::command<CrossPausingClient>;
    CrossPausingClient      *partner = nullptr;
    std::vector<std::string> texts;
    void
    on(Protocol::message &&msg) {
        texts.emplace_back(msg.text);
        if (partner)
            partner->pause_reading();
    }
};

/// Both peers send a line, both components readable before ONE loop pass.
void
send_both_then_one_pass(qb::io::tcp::socket &peer_one, qb::io::tcp::socket &peer_two) {
    EXPECT_EQ(peer_one.write("x\n", 2), 2);
    EXPECT_EQ(peer_two.write("y\n", 2), 2);
    std::this_thread::sleep_for(20ms); // both lines arrived: the pass's poll collects both readinesses
    async::run(EVRUN_NOWAIT);
}

} // namespace

// Two components readable in the same loop pass -- a first round, no pause, proves the pass dispatches both. Then the
// first one dispatched pauses the other from its handler, while the other's read readiness is already pending: the
// pause takes it back, and the other reads nothing until it resumes.
TEST(TextSessionPausedReads, APauseFromAnotherWatchersCallbackTakesBackItsPendingReadiness) {
    async::init();
    {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), SocketStatus::Done);
        const auto          port = listener.local_endpoint().port();
        CrossPausingClient  one, two;
        qb::io::tcp::socket peer_one, peer_two;
        ASSERT_EQ(one.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        ASSERT_EQ(listener.accept(peer_one), SocketStatus::Done);
        ASSERT_EQ(two.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        ASSERT_EQ(listener.accept(peer_two), SocketStatus::Done);
        one.start();
        two.start();
        pause_pump_for(20ms);

        send_both_then_one_pass(peer_one, peer_two);
        ASSERT_EQ(one.texts.size(), 1u) << "the case proves nothing: one pass did not dispatch both components";
        ASSERT_EQ(two.texts.size(), 1u) << "the case proves nothing: one pass did not dispatch both components";

        one.partner                = &two;
        two.partner                = &one;
        const std::size_t one_read = one.bytes_read(), two_read = two.bytes_read();
        send_both_then_one_pass(peer_one, peer_two);
        const bool          one_first = one.texts.size() == 2u;
        CrossPausingClient &first     = one_first ? one : two;
        CrossPausingClient &other     = one_first ? two : one;
        ASSERT_EQ(first.texts.size(), 2u) << "neither component was dispatched in the pass";
        EXPECT_EQ(other.texts.size(), 1u) << "a pause made from another watcher's callback let its pending readiness be dispatched";
        EXPECT_EQ(other.bytes_read(), one_first ? two_read : one_read) << "a component paused with its readiness pending read its transport";
        EXPECT_TRUE(other.reading_paused());
        pause_pump_for(50ms);
        EXPECT_EQ(other.texts.size(), 1u);

        other.partner = nullptr;
        first.partner = nullptr;
        other.resume_reading();
        EXPECT_TRUE(pump_until([&] { return other.texts.size() == 2u; }, 2s)) << "the resumed component never read its line";
        one.disconnect();
        two.disconnect();
        pause_pump_for(20ms);
    }
    async::listener::current.clear();
}

// The pause is input<>'s and io<>'s. A QUIC or HTTP/3 stream, on buffered_io, has none: a call does not compile.
static_assert(
    requires(TcpPauseSession &session) {
        session.pause_reading();
        session.resume_reading();
        session.reading_paused();
    }, "the control: an io<> session has the pause");
#ifdef QB_HAS_QUIC
static_assert(!requires(EchoTextQuicSession &session) { session.pause_reading(); }, "a QUIC stream has no pause_reading()");
static_assert(!requires(EchoTextQuicSession &session) { session.resume_reading(); }, "a QUIC stream has no resume_reading()");
#endif

namespace {

// A TLS server session paused from its accept, before its first handshake: its peer gives the handshake up.
struct handshake_pause_seen {
    int eofs         = 0;
    int disconnected = 0;
};
handshake_pause_seen handshake_pause;

class HandshakePauseServer;
class HandshakePauseSession : public use<HandshakePauseSession>::tcp::ssl::client<HandshakePauseServer> {
public:
    using Protocol = qb::protocol::text::command<HandshakePauseSession>;
    explicit HandshakePauseSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&) {}
    void
    on(async::event::eof &&) {
        ++handshake_pause.eofs;
    }
    void
    on(async::event::disconnected &&) {
        ++handshake_pause.disconnected;
    }
};
class HandshakePauseServer : public use<HandshakePauseServer>::tcp::ssl::server<HandshakePauseSession> {
public:
    void
    on(IOSession &session) {
        session.pause_reading(); // before the handshake: the reads it needs are made all the same
    }
};

enum class handshake_end { fin, reset, verify_fails };

/// The peer sends a handshake record's header and nothing after, then closes or resets -- or is a client whose check of
/// the server's certificate fails: the paused session ends as an unpaused one does, at once, not dispatched again.
void
expect_a_paused_handshake_to_end_with_its_peer(handshake_end end) {
    async::init();
    handshake_pause = {};
    {
        HandshakePauseServer server;
        server.transport().init(ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
        ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        const auto        port = server.transport().local_endpoint().port();
        std::atomic<bool> done{false};
        server.start();
        std::thread peer([&] {
            if (end == handshake_end::verify_fails) {
                qb::io::tcp::ssl::socket sock; // verifies the server: the local self-signed certificate fails that check
                (void) sock.connect_v4("127.0.0.1", port);
                sock.disconnect();
            } else {
                qb::io::tcp::socket sock;
                if (sock.connect_v4("127.0.0.1", port) == SocketStatus::Done) {
                    std::this_thread::sleep_for(30ms);
                    const unsigned char header[] = {0x16, 0x03, 0x01, 0x00, 0x40}; // a handshake record's header alone
                    EXPECT_EQ(sock.write(header, sizeof header), 5);
                    std::this_thread::sleep_for(60ms);
                    if (end == handshake_end::reset) {
                        ::linger zero{1, 0};
                        EXPECT_EQ(sock.set_optval(SOL_SOCKET, SO_LINGER, zero), 0);
                    }
                    sock.disconnect();
                }
            }
            done.store(true);
        });
        ASSERT_TRUE(pump_until([&] { return done.load(); }, 5s));
        peer.join();
        EXPECT_TRUE(pump_until([] { return handshake_pause.disconnected == 1; }, 2s))
            << "a session paused before its handshake outlived its peer's end";
        const int eofs = handshake_pause.eofs;
        pause_pump_for(100ms);
        EXPECT_LT(handshake_pause.eofs - eofs, 5) << "a session paused before its handshake was dispatched without bound";
        EXPECT_EQ(server.sessions().size(), 0u);
    }
    async::listener::current.clear();
}

/// TLS input the session holds above its socket (SSL_pending) when an eof hook pauses, the cap leaving the third line
/// there: the batch's replies published, then the resume -- the third line is read.
struct held_tls_seen {
    int  messages = 0;
    bool paused   = false;
};
held_tls_seen held_tls;

class HeldTlsServer;
class HeldTlsSession : public use<HeldTlsSession>::tcp::ssl::client<HeldTlsServer> {
public:
    using Protocol = qb::protocol::text::command<HeldTlsSession>;
    explicit HeldTlsSession(IOServer &server)
        : client(server) {}
    void
    on(Protocol::message &&) {
        ++held_tls.messages;
    }
    void
    on(async::event::eof &&) {
        if (!held_tls.paused && held_tls.messages == 2) { // the batch is in: hand it on, take no more meanwhile
            held_tls.paused = true;
            pause_reading();
        }
    }
};
class HeldTlsServer : public use<HeldTlsServer>::tcp::ssl::server<HeldTlsSession> {
public:
    void
    on(IOSession &session) {
        session.set_max_read_buffer_size(10000u); // two lines of the three one record holds
    }
};

} // namespace

TEST(TextSessionPausedHandshake, APausedSessionEndsWhenItsPeerClosesDuringTheHandshake) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    expect_a_paused_handshake_to_end_with_its_peer(handshake_end::fin);
}

TEST(TextSessionPausedHandshake, APausedSessionEndsWhenItsPeerResetsDuringTheHandshake) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    expect_a_paused_handshake_to_end_with_its_peer(handshake_end::reset);
}

TEST(TextSessionPausedHandshake, APausedSessionEndsWhenItsPeerFailsTheHandshake) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    expect_a_paused_handshake_to_end_with_its_peer(handshake_end::verify_fails);
}

// Output published, then resume_reading(): the readability the TLS resume gives back beside writability feeds what
// OpenSSL holds above the socket, as ready_to_read() does -- the third line is read.
TEST(TextSessionPausedHandshake, InputOpenSslHoldsIsReadWhenOutputIsPublishedBeforeTheResume) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    async::init();
    held_tls = {};
    {
        HeldTlsServer server;
        server.transport().init(ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem")));
        ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
        const auto        port = server.transport().local_endpoint().port();
        std::atomic<bool> finish{false}, done{false};
        server.start();
        std::thread peer([&] {
            qb::io::tcp::ssl::socket sock;
            sock.set_insecure();
            if (sock.connect_v4("127.0.0.1", port) == 0) {
                std::string request;
                for (int i = 0; i < 3; ++i)
                    request += std::string(4999, char('a' + i)) + "\n";
                EXPECT_EQ(sock.write(request.data(), request.size()), static_cast<int>(request.size())); // one record
                sock.set_nonblocking(true);
                std::vector<char> buffer(4096);
                while (!finish.load()) {
                    (void) sock.read(buffer.data(), buffer.size()); // takes the replies, sends nothing more
                    std::this_thread::sleep_for(1ms);
                }
                sock.disconnect();
            }
            done.store(true);
        });
        ASSERT_TRUE(pump_until([] { return held_tls.paused; }, 5s)) << "the case proves nothing: the session never paused";
        pause_pump_for(50ms);
        ASSERT_EQ(server.sessions().size(), 1u);
        auto &session = *server.sessions().begin()->second;
        ASSERT_GT(SSL_pending(session.transport().ssl_handle()), 0) << "the case proves nothing: OpenSSL holds nothing";
        session << "ok1\nok2\n";  // the batch's replies...
        session.resume_reading(); // ...then the next
        EXPECT_TRUE(pump_until([] { return held_tls.messages >= 3; }, 2s)) << "what OpenSSL held was never read after the resume";
        finish.store(true);
        (void) pump_until([&] { return done.load(); }, 5s);
        peer.join();
    }
    async::listener::current.clear();
}

// The peer ends its sending side while the session is paused with a whole message held, and reads on: the resume
// frames it and writes its answer before the session meets the end of stream -- the answer reaches the peer.
TEST(TextSessionPausedReads, AnAnswerToHeldInputReachesAPeerThatEndedItsSendingSide) {
    async::init();
    tcp_pause = {};
    {
        TcpPauseServer server;
        ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
        const auto        port = server.transport().local_endpoint().port();
        std::atomic<bool> half_close{false}, done{false};
        std::string       received;
        server.start();
        std::thread peer([&] {
            qb::io::tcp::socket sock;
            if (sock.connect_v4("127.0.0.1", port) == SocketStatus::Done) {
                EXPECT_EQ(sock.write("first\nsecond\n", 13), 13);
                while (!half_close.load())
                    std::this_thread::sleep_for(1ms);
                EXPECT_EQ(::shutdown(sock.native_handle(), QB_SD_SEND), 0); // its FIN: it sends nothing more, and reads on
                sock.set_nonblocking(true);
                char       buffer[256];
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < deadline) {
                    const int n = sock.read(buffer, sizeof buffer);
                    if (n > 0)
                        received.append(buffer, static_cast<std::size_t>(n));
                    else if (n == 0 || !qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
                        break;
                    else
                        std::this_thread::sleep_for(1ms);
                }
                sock.close();
            }
            done.store(true);
        });
        ASSERT_TRUE(pump_until([] { return !tcp_pause.texts.empty(); }, 5s)) << "the first message never came";
        pause_pump_for(50ms);
        ASSERT_EQ(server.sessions().size(), 1u);
        auto &session = *server.sessions().begin()->second;
        ASSERT_TRUE(session.reading_paused());
        half_close.store(true);
        pause_pump_for(100ms);
        session.resume_reading();
        EXPECT_TRUE(pump_until([] { return tcp_pause.disconnected == 1; }, 3s)) << "the session never met its peer's end";
        EXPECT_TRUE(pump_until([&] { return done.load(); }, 5s));
        peer.join();
        EXPECT_EQ(tcp_pause.texts, (std::vector<std::string>{"first", "second"}));
        EXPECT_EQ(received, "first\nsecond\n") << "the answer to what the input held never left";
    }
    async::listener::current.clear();
}
