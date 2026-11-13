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
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_pump.h"

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

// =============================================================================
// A session's read cap: read up to it, refused only past it, nothing left inside TLS (Huly QB-86)
// =============================================================================
//
// Until 3.4 a session could not reach its transport's read cap at all -- the transport is a private base of
// `async::tcp::client`, which re-exposed the write cap alone -- and the cap, where a transport had one, was enforced a
// whole read bucket at a time: under a cap below the bucket (64 KiB) no read was ever made, and a stream within a
// bucket of its cap was refused although its octets fit. The session below sets its own cap when it is built. Both
// ends live on the test thread: the server and its session on this thread's loop, the client a raw socket the case
// writes to -- over TLS one write of at most 16384 octets is one record -- so every octet is where the case put it.
//
// What is specific to TLS: a record is decrypted whole, so a read that takes less than the record leaves the rest
// inside the TLS session, and the socket, emptied, raises no event for it. The io layer dispatches the component
// again while its transport holds such octets (`transport::stcp::held_input()`): with room they are read, with none
// the read is refused, as it is for octets still in a TCP socket. Without that they would wait for the peer's next
// record -- for ever, when the peer is itself waiting for the answer to what it sent.

namespace tls_text_roundtrip_test {

constexpr std::size_t kLine      = 100u;                        // a line, its delimiter included
constexpr std::size_t kBucket    = QB_DEFAULT_READ_BUFFER_SIZE; // what one read asks for at most
constexpr std::size_t kMaxRecord = 16384u;                      // the most plaintext one TLS record carries

constexpr int kBufferOverflow = static_cast<int>(async::event::disconnect_reason::buffer_overflow);

/// Line `index` of a case: its number, then a filler that differs from its neighbours', `kLine` octets in all.
std::string
line(std::size_t index) {
    std::string text = std::to_string(index) + ':';
    text.resize(kLine - 1u, static_cast<char>('a' + index % 26u));
    text += '\n';
    return text;
}

/// Lines `0` to `count - 1`, one after the other.
std::string
lines(std::size_t count) {
    std::string all;
    for (std::size_t i = 0; i < count; ++i)
        all += line(i);
    return all;
}

/// What a socket's receive buffer holds, or nothing when the socket cannot say.
std::optional<std::size_t>
waiting_in_socket(qb::io::tcp::socket const &sock) {
#ifdef _WIN32
    u_long held = 0;
#else
    int held = 0;
#endif
    if (::QB_IOCTLSOCKET(sock.native_handle(), FIONREAD, &held) != 0)
        return std::nullopt;
    return static_cast<std::size_t>(held);
}

/// What the reader of a case saw. Everything runs on the test thread, so plain counters do.
struct ReadCapProbe {
    std::size_t cap                       = 0; ///< the read cap the session sets when it is built
    std::size_t lines                     = 0; ///< lines delivered
    std::size_t misordered                = 0; ///< of them, those that were not the next line of the case
    std::size_t buffered                  = 0; ///< what the input buffer held after the last read pass
    std::size_t largest                   = 0; ///< the most it ever held, at a delivery or after a read pass
    std::size_t held_with_an_empty_socket = 0; ///< deliveries made with octets inside the TLS session and none in the socket
    std::size_t stop_after                = 0; ///< the handler of that line stops the session (0: none does)
    int         disconnections            = 0;
    int         reason                    = 0; ///< of the disconnection
    std::size_t buffered_at_disconnection = 0;

    /// One line delivered to a reader whose input buffer holds `in_buffer` octets, that line included.
    void
    delivered(std::string_view text, std::size_t in_buffer) {
        if (line(lines).compare(0u, kLine - 1u, text) != 0)
            ++misordered;
        ++lines;
        if (in_buffer > largest)
            largest = in_buffer;
    }

    /// A read pass ended with `in_buffer` octets no line was made of.
    void
    left(std::size_t in_buffer) {
        buffered = in_buffer;
        if (in_buffer > largest)
            largest = in_buffer;
    }

    void
    disconnected(int why, std::size_t in_buffer) {
        ++disconnections;
        reason                    = why;
        buffered_at_disconnection = in_buffer;
    }
};

ReadCapProbe g_read_cap;

template <typename Transport, typename Accept>
class ReadCapServer;

/// A server session that sets its own read cap -- what it could not do while only the write cap was re-exposed.
template <typename Transport, typename Accept>
class ReadCapSession : public async::tcp::client<ReadCapSession<Transport, Accept>, Transport, ReadCapServer<Transport, Accept>> {
    using base_t = async::tcp::client<ReadCapSession<Transport, Accept>, Transport, ReadCapServer<Transport, Accept>>;

public:
    using Protocol = qb::protocol::text::command<ReadCapSession>;

    explicit ReadCapSession(ReadCapServer<Transport, Accept> &server)
        : base_t(server) {
        this->set_max_read_buffer_size(g_read_cap.cap);
    }

    void
    on(typename Protocol::message &&msg) {
        g_read_cap.delivered(msg.text, this->in().size());
        if constexpr (Transport::is_secure()) {
            if (SSL_pending(this->transport().ssl_handle()) > 0 && waiting_in_socket(this->transport()) == std::optional<std::size_t>{0u})
                ++g_read_cap.held_with_an_empty_socket;
        }
        if (g_read_cap.lines == g_read_cap.stop_after)
            this->stop();
    }

    void
    on(async::event::pending_read &&event) {
        g_read_cap.left(event.bytes);
    }

    void
    on(async::event::disconnected const &event) {
        g_read_cap.disconnected(event.reason, this->in().size());
    }
};

template <typename Transport, typename Accept>
class ReadCapServer : public async::tcp::server<ReadCapServer<Transport, Accept>, ReadCapSession<Transport, Accept>, Accept> {};

/// A listening server whose one session set `cap` as its read cap, and the raw client end the case writes to.
template <typename Transport, typename Accept>
class CappedSession {
    using Peer = typename Transport::transport_io_type;

    ReadCapServer<Transport, Accept> _server;
    Peer                             _peer;

public:
    [[nodiscard]] ::testing::AssertionResult
    open(std::size_t cap) {
        g_read_cap     = ReadCapProbe{};
        g_read_cap.cap = cap;
        async::init();
        if constexpr (Accept::is_secure()) {
            if (!require_ssl_files())
                return ::testing::AssertionFailure() << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
            _server.transport().init(ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
        }
        if (_server.transport().listen_v4(0, "127.0.0.1") != 0)
            return ::testing::AssertionFailure() << "listen_v4 failed";
        const auto port = _server.transport().local_endpoint().port();
        _server.start();

        if constexpr (Transport::is_secure()) {
            _peer.set_insecure(); // self-signed local cert
            const int ret = _peer.n_connect(qb::io::endpoint{"127.0.0.1", port}, "localhost");
            if (ret != 0 && !qb::io::socket_no_error(qb::io::socket::get_last_errno()))
                return ::testing::AssertionFailure() << "n_connect failed: " << ret;
        } else {
            if (_peer.connect_v4("127.0.0.1", port) != 0)
                return ::testing::AssertionFailure() << "connect_v4 failed";
            _peer.set_nonblocking(true);
        }
        // The session's side of the handshake runs on this thread's loop, the client's here.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            async::run_for(1ms);
            bool up = _server.session_count() == 1u;
            if constexpr (Transport::is_secure()) {
                if (!_peer.handshake_complete() && _peer.handshake_status() < 0)
                    return ::testing::AssertionFailure() << "client handshake failed: " << _peer.get_last_ssl_error_string();
                up = up && _peer.handshake_complete();
            }
            if (up)
                return ::testing::AssertionSuccess();
            if (std::chrono::steady_clock::now() >= deadline)
                return ::testing::AssertionFailure() << "no session within the deadline";
        }
    }

    /// The session, while the server holds it.
    [[nodiscard]] auto
    session() {
        return _server.session_count() ? _server.sessions().begin()->second : nullptr;
    }

    /// The client writes `octets`, at most `kMaxRecord` per write -- over TLS, one record each -- and the loop is
    /// pumped while its socket takes no more.
    [[nodiscard]] ::testing::AssertionResult
    send(std::string_view octets) {
        const auto  deadline = std::chrono::steady_clock::now() + 5s;
        std::size_t sent     = 0;
        while (sent < octets.size()) {
            const std::size_t piece = octets.size() - sent < kMaxRecord ? octets.size() - sent : kMaxRecord;
            const int         ret   = _peer.write(octets.data() + sent, piece);
            if (ret > 0) {
                sent += static_cast<std::size_t>(ret);
                continue;
            }
            if (ret < 0 && !qb::io::socket_no_error(qb::io::socket::get_last_errno()))
                return ::testing::AssertionFailure() << "the client's write failed after " << sent << " octets";
            if (std::chrono::steady_clock::now() >= deadline)
                return ::testing::AssertionFailure() << "the client's socket took " << sent << " of " << octets.size() << " octets";
            async::run_for(1ms);
        }
        return ::testing::AssertionSuccess();
    }

    /// The client ends its stream: a `close_notify` and a FIN over TLS, a FIN over TCP.
    void
    end_of_stream() {
        static_cast<void>(_peer.disconnect());
        _peer.close();
    }

    ~CappedSession() {
        static_cast<void>(_peer.disconnect());
        for (int i = 0; i < 10; ++i)
            async::run_for(1ms);
        async::listener::current.clear();
    }
};

/// A cap below the read bucket: the session reads up to it, whatever the stream, and is refused only past it.
template <typename Transport, typename Accept>
void
expect_a_cap_below_the_bucket_reads_up_to_it() {
    constexpr std::size_t kCap   = 1024u;
    constexpr std::size_t kLines = 40u; // 4000 octets: four times the cap, in one write
    ASSERT_LT(kCap, kBucket);

    CappedSession<Transport, Accept> io;
    ASSERT_TRUE(io.open(kCap));
    ASSERT_NE(io.session(), nullptr);
    EXPECT_EQ(io.session()->max_read_buffer_size(), kCap) << "the session reads back the cap it set";

    ASSERT_TRUE(io.send(lines(kLines)));
    EXPECT_TRUE(pump_until([] { return g_read_cap.lines >= kLines || g_read_cap.disconnections > 0; }, 5s));
    EXPECT_EQ(g_read_cap.lines, kLines) << "a session whose cap is below the read bucket reads its stream";
    EXPECT_EQ(g_read_cap.misordered, 0u);
    EXPECT_EQ(g_read_cap.disconnections, 0) << "disconnected with reason " << g_read_cap.reason;
    EXPECT_LE(g_read_cap.largest, kCap) << "the input buffer never held more than the cap";

    // A line longer than the cap can never be framed: the session reads up to its cap, and is refused there.
    ASSERT_TRUE(io.send(std::string(kCap + 476u, 'x')));
    EXPECT_TRUE(pump_until([] { return g_read_cap.disconnections > 0; }, 5s)) << "a stream past the cap was never refused";
    EXPECT_EQ(g_read_cap.disconnections, 1);
    EXPECT_EQ(g_read_cap.reason, kBufferOverflow);
    EXPECT_EQ(g_read_cap.buffered_at_disconnection, kCap) << "read up to the cap, and not past it";
}

/// A stream that fills exactly to the cap is read whole and kept; one more octet is refused.
template <typename Transport, typename Accept>
void
expect_a_stream_that_fills_exactly_to_the_cap_is_read_whole() {
    constexpr std::size_t kCap = kBucket + 1000u; // past one bucket, the room left is under a bucket: 1000 octets

    CappedSession<Transport, Accept> io;
    ASSERT_TRUE(io.open(kCap));

    ASSERT_TRUE(io.send(std::string(kCap, 'y'))); // no delimiter: nothing is consumed
    EXPECT_TRUE(pump_until([] { return g_read_cap.buffered >= kCap || g_read_cap.disconnections > 0; }, 5s));
    EXPECT_EQ(g_read_cap.buffered, kCap) << "the stream that fits the cap was not read whole";
    // It fits: the session keeps it and stays connected, for as long as nothing more arrives.
    static_cast<void>(pump_until([] { return g_read_cap.disconnections > 0; }, 100ms));
    EXPECT_EQ(g_read_cap.disconnections, 0) << "refused with " << g_read_cap.buffered_at_disconnection << " octets under a cap of " << kCap;
    ASSERT_NE(io.session(), nullptr);
    EXPECT_EQ(io.session()->in().size(), kCap);

    ASSERT_TRUE(io.send("z")); // one octet more
    EXPECT_TRUE(pump_until([] { return g_read_cap.disconnections > 0; }, 5s)) << "the octet past the cap was never refused";
    EXPECT_EQ(g_read_cap.disconnections, 1);
    EXPECT_EQ(g_read_cap.reason, kBufferOverflow);
    EXPECT_EQ(g_read_cap.buffered_at_disconnection, kCap) << "nothing was read past the cap";
    EXPECT_EQ(g_read_cap.largest, kCap);
}

/// A buffer at its cap is refused at the next read, whatever brings that read: the peer's end of stream ends the
/// session as an overflow, not as a close by the peer. Kept as it was, and written down.
template <typename Transport, typename Accept>
void
expect_the_peers_end_of_stream_at_the_cap_ends_the_session_as_an_overflow() {
    constexpr std::size_t kCap = 1000u;

    CappedSession<Transport, Accept> io;
    ASSERT_TRUE(io.open(kCap));
    ASSERT_TRUE(io.send(std::string(kCap, 'y'))); // no delimiter: nothing is consumed
    EXPECT_TRUE(pump_until([] { return g_read_cap.buffered >= kCap || g_read_cap.disconnections > 0; }, 5s));
    static_cast<void>(pump_until([] { return g_read_cap.disconnections > 0; }, 50ms));
    ASSERT_EQ(g_read_cap.disconnections, 0) << "a buffer at its cap is kept while nothing more arrives";

    io.end_of_stream();
    EXPECT_TRUE(pump_until([] { return g_read_cap.disconnections > 0; }, 5s)) << "the peer's end of stream was never read";
    EXPECT_EQ(g_read_cap.reason, kBufferOverflow) << "the read the end of stream brought finds the buffer at its cap";
    EXPECT_EQ(g_read_cap.buffered_at_disconnection, kCap);
}

/// A reader on `input<>` alone over a TLS stream it inherits in the open: the composition a user writes by hand, and
/// the other base that reads -- `async::tcp::client` is built on `io<>`.
class TlsLineReader
    : public async::input<TlsLineReader>
    , public transport::stcp {
public:
    using Protocol = qb::protocol::text::command<TlsLineReader>;

    ReadCapProbe seen;
    std::size_t  held_input_disagreements = 0; ///< deliveries at which `held_input()` was not what OpenSSL counts

    TlsLineReader() {
        static_cast<void>(this->template switch_protocol<Protocol>(*this));
    }

    ~TlsLineReader() {
        this->_async_event.stop(); // before the transport, a base listed after the watcher's, closes the descriptor
    }

    void
    on(Protocol::message &&msg) {
        seen.delivered(msg.text, in().size());
        const auto held = static_cast<std::size_t>(SSL_pending(transport().ssl_handle()));
        if (held_input() != held)
            ++held_input_disagreements;
        if (held > 0u && waiting_in_socket(transport()) == std::optional<std::size_t>{0u})
            ++seen.held_with_an_empty_socket;
    }

    void
    on(async::event::pending_read &&event) {
        seen.left(event.bytes);
    }

    void
    on(async::event::disconnected &&event) {
        seen.disconnected(event.reason, in().size());
    }
};

} // namespace tls_text_roundtrip_test

TEST(TlsTextRoundtrip, ASessionUnderAReadCapBelowTheBucketReadsUpToItOverTls) {
    tls_text_roundtrip_test::expect_a_cap_below_the_bucket_reads_up_to_it<transport::stcp, transport::saccept>();
}

TEST(TlsTextRoundtrip, ASessionUnderAReadCapBelowTheBucketReadsUpToItOverTcp) {
    tls_text_roundtrip_test::expect_a_cap_below_the_bucket_reads_up_to_it<transport::tcp, transport::accept>();
}

TEST(TlsTextRoundtrip, ASessionReadsAStreamThatFillsExactlyToItsReadCapOverTls) {
    tls_text_roundtrip_test::expect_a_stream_that_fills_exactly_to_the_cap_is_read_whole<transport::stcp, transport::saccept>();
}

TEST(TlsTextRoundtrip, ASessionReadsAStreamThatFillsExactlyToItsReadCapOverTcp) {
    tls_text_roundtrip_test::expect_a_stream_that_fills_exactly_to_the_cap_is_read_whole<transport::tcp, transport::accept>();
}

// One TLS record of thirty lines reaches a session whose read cap holds ten. The first read takes the cap; the other
// twenty lines are decrypted and held inside the TLS session, and the socket is empty -- the case checks that it met
// exactly that. The peer sends nothing more: only the io layer's own dispatch can read them. Every line is delivered,
// in order, the buffer never past its cap, the session still connected.
TEST(TlsTextRoundtrip, OctetsARecordLeavesInsideTheTlsSessionAreReadOnceTheProtocolConsumes) {
    using namespace tls_text_roundtrip_test;
    constexpr std::size_t kCap   = 1024u;
    constexpr std::size_t kLines = 30u; // 3000 octets: one record, three times the cap

    CappedSession<transport::stcp, transport::saccept> io;
    ASSERT_TRUE(io.open(kCap));
    ASSERT_LE(kLines * kLine, kMaxRecord);
    ASSERT_TRUE(io.send(lines(kLines)));

    EXPECT_TRUE(pump_until([] { return g_read_cap.lines >= kLines || g_read_cap.disconnections > 0; }, 5s))
        << "octets held inside the TLS session were never read: " << g_read_cap.lines << " of " << kLines << " lines delivered";
    EXPECT_EQ(g_read_cap.lines, kLines);
    EXPECT_EQ(g_read_cap.misordered, 0u);
    EXPECT_EQ(g_read_cap.disconnections, 0) << "disconnected with reason " << g_read_cap.reason;
    EXPECT_LE(g_read_cap.largest, kCap) << "the input buffer never held more than the cap";
    EXPECT_GT(g_read_cap.held_with_an_empty_socket, 0u)
        << "the case never had octets held inside the TLS session with nothing left in the socket";
    ASSERT_NE(io.session(), nullptr);
    EXPECT_EQ(SSL_pending(io.session()->transport().ssl_handle()), 0) << "octets are still held inside the TLS session";
}

TEST(TlsTextRoundtrip, ThePeersEndOfStreamAtTheReadCapEndsTheSessionAsAnOverflowOverTls) {
    tls_text_roundtrip_test::expect_the_peers_end_of_stream_at_the_cap_ends_the_session_as_an_overflow<transport::stcp, transport::saccept>();
}

TEST(TlsTextRoundtrip, ThePeersEndOfStreamAtTheReadCapEndsTheSessionAsAnOverflowOverTcp) {
    tls_text_roundtrip_test::expect_the_peers_end_of_stream_at_the_cap_ends_the_session_as_an_overflow<transport::tcp, transport::accept>();
}

// A session that stops itself in a handler is not dispatched for what its TLS stream holds -- the dispatch is for a
// started component -- and its start() reads it: the record's last twenty lines wait inside the TLS session, with
// nothing in the socket, for as long as the session is stopped.
TEST(TlsTextRoundtrip, AStoppedSessionIsNotDispatchedForWhatItsTlsStreamHoldsAndItsStartReadsIt) {
    using namespace tls_text_roundtrip_test;
    constexpr std::size_t kCap   = 1024u;
    constexpr std::size_t kLines = 30u; // 3000 octets: one record
    constexpr std::size_t kFirst = 10u; // the lines the first read, of 1024 octets, holds whole

    CappedSession<transport::stcp, transport::saccept> io;
    ASSERT_TRUE(io.open(kCap));
    g_read_cap.stop_after = kFirst;
    ASSERT_TRUE(io.send(lines(kLines)));

    EXPECT_TRUE(pump_until([] { return g_read_cap.lines >= kFirst; }, 5s));
    static_cast<void>(pump_until([] { return g_read_cap.lines > kFirst; }, 100ms));
    EXPECT_EQ(g_read_cap.lines, kFirst) << "a session that stopped itself was dispatched again";
    auto session = io.session();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(SSL_pending(session->transport().ssl_handle()), static_cast<int>(kLines * kLine - kCap))
        << "what the first read left is held inside the TLS session";

    g_read_cap.stop_after = 0u;
    session->start();
    EXPECT_TRUE(pump_until([] { return g_read_cap.lines >= kLines || g_read_cap.disconnections > 0; }, 5s))
        << "start() did not read what the TLS stream holds: " << g_read_cap.lines << " of " << kLines << " lines delivered";
    EXPECT_EQ(g_read_cap.lines, kLines);
    EXPECT_EQ(g_read_cap.misordered, 0u);
    EXPECT_EQ(g_read_cap.disconnections, 0) << "disconnected with reason " << g_read_cap.reason;
}

// The same for a component built on `input<>` -- the other base that reads -- over a TLS stream it inherits in the
// open, where `held_input()` can be read: it is what OpenSSL counts at every delivery, nothing once the record is read,
// and nothing for a transport that has no session.
TEST(TlsTextRoundtrip, AnInputComponentReadsWhatItsTlsStreamHolds) {
    using namespace tls_text_roundtrip_test;
    constexpr std::size_t kCap   = 1024u;
    constexpr std::size_t kLines = 30u;
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
    EXPECT_EQ(transport::stcp{}.held_input(), 0u) << "a transport with no TLS session holds nothing";

    async::init();
    qb::io::tcp::ssl::listener listener{ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"))};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    qb::io::test::tls_pair pair;
    pair.client.set_insecure(); // self-signed local cert
    ASSERT_TRUE(qb::io::test::open_tls_connection(listener, listener.local_endpoint().port(), pair));
    ASSERT_TRUE(qb::io::test::complete_tls_handshake(pair));
    {
        TlsLineReader reader;
        reader.transport() = std::move(pair.server);
        reader.set_max_read_buffer_size(kCap);
        reader.start();

        const std::string record = lines(kLines);
        ASSERT_EQ(pair.client.write(record.data(), record.size()), static_cast<int>(record.size())); // one record
        EXPECT_TRUE(pump_until([&] { return reader.seen.lines >= kLines || reader.seen.disconnections > 0; }, 5s))
            << "octets held inside the TLS session were never read: " << reader.seen.lines << " of " << kLines << " lines delivered";
        EXPECT_EQ(reader.seen.lines, kLines);
        EXPECT_EQ(reader.seen.misordered, 0u);
        EXPECT_EQ(reader.seen.disconnections, 0) << "disconnected with reason " << reader.seen.reason;
        EXPECT_LE(reader.seen.largest, kCap);
        EXPECT_GT(reader.seen.held_with_an_empty_socket, 0u)
            << "the case never had octets held inside the TLS session with nothing left in the socket";
        EXPECT_EQ(reader.held_input_disagreements, 0u) << "held_input() is what the TLS session holds decrypted";
        EXPECT_EQ(reader.held_input(), 0u) << "nothing is held once the record is read";

        // Past the cap: a record of one line longer than the cap fills the buffer, and what the session still holds of it
        // is refused as octets still in a socket are -- the component is dispatched for them, and has no room.
        const std::string too_long(kCap + 476u, 'x');
        ASSERT_EQ(pair.client.write(too_long.data(), too_long.size()), static_cast<int>(too_long.size()));
        EXPECT_TRUE(pump_until([&] { return reader.seen.disconnections > 0; }, 5s)) << "a stream past the cap was never refused";
        EXPECT_EQ(reader.seen.reason, kBufferOverflow);
        EXPECT_EQ(reader.seen.buffered_at_disconnection, kCap) << "read up to the cap, and not past it";
    }
    static_cast<void>(pair.client.disconnect());
    async::listener::current.clear();
}
