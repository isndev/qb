/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-text-roundtrip.cpp
 * @brief End-to-end TLS over the async session stack — a command-protocol echo round-trip.
 *
 * Promoted from test-async-io.cpp's `SSLCommunication`. It exercises TLS not at the raw-socket level
 * (that is system/tcp/ssl-socket-loopback.cpp) but through the full `use<>::tcp::ssl::{server,client}`
 * session stack with the `qb::protocol::text::command` framing: a secure server echoes every line a
 * secure client sends, and both sides count exactly `kIterations` framed messages. This is the
 * session-layer proof that the TLS transport carries application data transparently.
 *
 * Hardening over the original (per the restructure spec §2/§7):
 *   - cert presence is a HARD prerequisite (`ASSERT_TRUE(require_ssl_files())`), not a `GTEST_SKIP`
 *     mask — the harness ships the cert; the locator is the shared `tests/shared/ssl_fixtures.h`.
 *   - NO fixed port: the listener binds `:0` and the client connects to the kernel-assigned port.
 *   - NO two-thread / two-loop `async::run(EVRUN_ONCE)` + `sleep_for(20ms)` race: server and client
 *     share ONE event loop driven by the bounded `qb::io::test::pump_until`, which fails LOUDLY on a
 *     stall instead of silently exhausting a fixed iteration budget.
 *   - the shipped `std::cout << "Starting…"` debug print is gone.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <cstddef>
#include <thread>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/ssl_fixtures.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

constexpr const char        kMessage[]  = "Hello, Secure Text Protocol!";
constexpr const std::size_t kIterations = 10;

std::atomic<std::size_t> g_server_echoed{0};
std::atomic<std::size_t> g_client_received{0};

class SecureServer;

// Server-side per-connection session: echoes each framed line back to the client.
class SecureServerClient : public use<SecureServerClient>::tcp::ssl::client<SecureServer> {
public:
    using Protocol = qb::protocol::text::command<SecureServerClient>;

    explicit SecureServerClient(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(kMessage) - 1);
        *this << msg.text << Protocol::end;
        ++g_server_echoed;
    }
};

class SecureServer : public use<SecureServer>::tcp::ssl::server<SecureServerClient> {
public:
    std::size_t connection_count = 0u;

    void
    on(IOSession &) {
        ++connection_count;
    }
};

// Client session: counts the echoes the server sends back.
class SecureClient : public use<SecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<SecureClient>;

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(kMessage) - 1);
        ++g_client_received;
    }
};

} // namespace

TEST(TlsTextRoundtrip, SecureSessionEchoesEveryFramedMessage) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    async::init();
    g_server_echoed   = 0;
    g_client_received = 0;

    // Secure server on an ephemeral loopback port.
    SecureServer server;
    server.transport().init(ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    // Secure client on a SEPARATE thread with its own event loop. A TLS client connect
    // is a synchronous handshake (connect_v4 blocks until SSL_connect completes); running
    // it on this thread would deadlock, because the server lives on THIS loop and cannot
    // answer the handshake while we block. With the client on its own thread+loop, the
    // server loop is pumped concurrently on the main thread, so the handshake completes
    // and both sides progress. (Same proven pattern as the SecureTcpPolicy session test.)
    std::atomic<bool> client_thread_ok{false};
    std::thread       worker([&] {
        async::init();
        SecureClient client;
        client.transport().set_insecure(); // self-signed local cert
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        client.start();

        for (std::size_t i = 0; i < kIterations; ++i)
            client << kMessage << '\n';

        const bool ok = pump_until([&] { return g_client_received.load() >= kIterations; }, 5s);
        EXPECT_TRUE(ok) << "client never received all echoes: " << g_client_received.load() << " of " << kIterations;
        client_thread_ok.store(ok);
    });

    const bool done = pump_until([&] { return g_server_echoed.load() >= kIterations && g_client_received.load() >= kIterations; }, 5s);
    EXPECT_TRUE(done) << "secure echo round-trip stalled: server echoed " << g_server_echoed.load() << ", client received "
                      << g_client_received.load() << " of " << kIterations;

    worker.join();

    EXPECT_TRUE(client_thread_ok.load());
    EXPECT_EQ(g_server_echoed.load(), kIterations);
    EXPECT_EQ(g_client_received.load(), kIterations);
    EXPECT_EQ(server.connection_count, 1u);

    async::listener::current.clear();
}

// =============================================================================
// An empty write arm is answered by eos, over TLS as over TCP (Huly QB-1176)
// =============================================================================
//
// `ready_to_write()` with nothing queued is how a session asks to be told that its output has drained:
// the write path runs, finds nothing to send and reports `event::eos`. Over TCP that held because a
// 0-byte send() answers 0. Over TLS it did not: the write path handed SSL_write() 0 bytes, which fails,
// and the io layer read the failure through whatever errno the thread held last -- a would-block left
// by an earlier read re-armed the write on every turn (the session never heard eos and the loop spun),
// anything else disposed a healthy connection. The server session below arms an empty write when the
// client sends `arm`, must then hear eos exactly once while still connected, and must answer the
// client's next line, `ping`, on the same connection. The TCP case is the control: the same harness
// and the same expectations, met before the fix too.

namespace {

struct EmptyArmProbe {
    std::atomic<int> empty_arms{0};       // arms made with nothing queued
    std::atomic<int> eos_after_arm{0};    // eos heard after the arm and before `ping`
    std::atomic<int> lost_before_ping{0}; // disconnections before `ping`
    std::atomic<int> pinged{0};           // `ping` received on the session that armed
    std::atomic<int> ponged{0};           // the answer, received by the client

    void
    reset() noexcept {
        empty_arms       = 0;
        eos_after_arm    = 0;
        lost_before_ping = 0;
        pinged           = 0;
        ponged           = 0;
    }
};

EmptyArmProbe g_empty_arm;

template <typename Transport, typename Accept>
class EmptyArmServer;

template <typename Transport, typename Accept>
class EmptyArmSession : public async::tcp::client<EmptyArmSession<Transport, Accept>, Transport, EmptyArmServer<Transport, Accept>> {
    using base_t = async::tcp::client<EmptyArmSession<Transport, Accept>, Transport, EmptyArmServer<Transport, Accept>>;
    bool _armed  = false;

public:
    using Protocol = qb::protocol::text::command<EmptyArmSession>;

    explicit EmptyArmSession(EmptyArmServer<Transport, Accept> &server)
        : base_t(server) {}

    void
    on(typename Protocol::message &&msg) {
        if (msg.text == "arm") {
            EXPECT_EQ(this->out().size(), 0u) << "the arm must find nothing queued";
            _armed = true;
            ++g_empty_arm.empty_arms;
            this->ready_to_write();
        } else if (msg.text == "ping") {
            ++g_empty_arm.pinged;
            *this << "pong" << Protocol::end;
        }
    }

    void
    on(async::event::eos &&) {
        if (_armed && g_empty_arm.pinged.load() == 0)
            ++g_empty_arm.eos_after_arm;
    }

    void
    on(async::event::disconnected const &) {
        if (g_empty_arm.pinged.load() == 0)
            ++g_empty_arm.lost_before_ping;
    }
};

template <typename Transport, typename Accept>
class EmptyArmServer : public async::tcp::server<EmptyArmServer<Transport, Accept>, EmptyArmSession<Transport, Accept>, Accept> {};

template <typename Transport>
class EmptyArmClient : public async::tcp::client<EmptyArmClient<Transport>, Transport> {
public:
    using Protocol = qb::protocol::text::command<EmptyArmClient>;

    void
    on(typename Protocol::message &&msg) {
        if (msg.text == "pong")
            ++g_empty_arm.ponged;
    }
};

template <typename Transport, typename Accept>
void
expect_empty_arm_answered_by_eos() {
    g_empty_arm.reset();
    async::init();

    EmptyArmServer<Transport, Accept> server;
    if constexpr (Accept::is_secure())
        server.transport().init(ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    // The client on its own thread and loop: a TLS connect_v4() is a blocking handshake the server,
    // on THIS loop, must answer (see SecureSessionEchoesEveryFramedMessage).
    std::atomic<bool> client_done{false};
    std::thread       worker([&] {
        async::init();
        EmptyArmClient<Transport> client;
        if constexpr (Transport::is_secure())
            client.transport().set_insecure(); // self-signed local cert
        const bool connected = client.transport().connect_v4("127.0.0.1", port) == SocketStatus::Done;
        EXPECT_TRUE(connected);
        if (connected) {
            client.start();
            client << "arm" << '\n';
            const bool answered =
                pump_until([] { return g_empty_arm.eos_after_arm.load() > 0 || g_empty_arm.lost_before_ping.load() > 0; }, 5s);
            EXPECT_TRUE(answered) << "the empty arm was answered neither by eos nor by a disconnection within 5 s";
            if (answered && g_empty_arm.lost_before_ping.load() == 0) {
                client << "ping" << '\n';
                EXPECT_TRUE(pump_until([] { return g_empty_arm.ponged.load() > 0; }, 5s)) << "the session did not answer after its eos";
            }
        }
        client_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return client_done.load(); }, 15s)) << "the client thread never finished";
    worker.join();

    EXPECT_EQ(g_empty_arm.empty_arms.load(), 1);
    EXPECT_EQ(g_empty_arm.eos_after_arm.load(), 1) << "an empty arm must be answered by exactly one eos";
    EXPECT_EQ(g_empty_arm.lost_before_ping.load(), 0) << "an empty arm must not end the connection";
    EXPECT_EQ(g_empty_arm.pinged.load(), 1) << "the session that armed must still read the next line";
    EXPECT_EQ(g_empty_arm.ponged.load(), 1) << "the session that armed must still answer it";

    async::listener::current.clear();
}

} // namespace

TEST(TlsTextRoundtrip, EmptyWriteArmIsAnsweredByEosOverTls) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
    expect_empty_arm_answered_by_eos<transport::stcp, transport::saccept>();
}

TEST(TlsTextRoundtrip, EmptyWriteArmIsAnsweredByEosOverTcp) {
    expect_empty_arm_answered_by_eos<transport::tcp, transport::accept>();
}
