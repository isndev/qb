/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file system/tcp/secure-transport-loopback.cpp
 * @brief `qb::io::transport::saccept` + `qb::io::transport::stcp` — the SECURE acceptor and SECURE
 *        stream mixins, over a real loopback TLS handshake.
 *
 * ssl-socket-loopback.cpp drives the raw `qb::io::tcp::ssl::{socket,listener}`. This test wraps the
 * exact same primitives in the two transport mixins that production code actually composes, exercising
 * their thin-but-low-coverage surfaces directly:
 *
 *   - `transport::saccept` (transport/saccept.h): `transport()` exposes the inner `ssl::listener` so the
 *     SSL_CTX is wired with `init(create_server_context(...))` + `set_supported_alpn_protocols` +
 *     `listen_v4`. `read()` accepts a pending secure connection and returns the accepted native handle;
 *     `getAccepted()` is the `ssl::socket` on which the server-side handshake is driven to completion;
 *     `flush()` releases the moved-out accepted handle; `close()` shuts the listener; `is_secure()` is
 *     a compile-time `true`. A no-pending-connection `read()` returns `(size_t)-1`.
 *
 *   - `transport::stcp` (transport/stcp.h, `: stream<ssl::socket>`): the client uses `transport()` to
 *     reach the `ssl::socket` (`set_insecure()` for the self-signed cert, `set_sni_hostname`,
 *     `set_alpn_protocols`, blocking `connect_v4` which completes the handshake), then drives the
 *     stream half: `publish()` + `write()` to send an encrypted line, and `read()` (the SSL-pending-
 *     aware override) draining decrypted bytes into `in()` / `pendingRead()`. The round-trip proves the
 *     stcp `read()` correctly retrieves SSL-buffered application data.
 *
 *   - `transport::stcp` under a read cap (Huly QB-86): `read()` takes at most the room left under
 *     `max_read_buffer_size()` -- a cap below the read bucket still reads, a stream that fits its cap is read
 *     whole -- and what a record holds past that room stays inside the TLS session, read later with nothing
 *     more from the socket. Both ends are driven by the test thread (shared/tls_pump.h), so every record and
 *     every read happens where the case writes it.
 *
 * Both POSIX and Windows keep the accepted socket alive until the client has finished.
 * The synchronous client uses a fixture deadline covering TCP and TLS; the server polls its
 * nonblocking handshake. SSL is gated at CMake (`REQUIRES ssl`); the shipped self-signed
 * `cert.pem`/`key.pem` are located via the shared ssl_fixtures helper.
 *
 * Signatures relied on:
 *   transport::saccept: ssl::listener& transport(); std::size_t read(); void flush(std::size_t);
 *                       void close(); ssl::socket& getAccepted(); static constexpr bool is_secure();
 *   transport::stcp:    ssl::socket& transport(); int read(); int write(); char* publish(const char*, size_t);
 *                       input_buffer& in(); std::size_t pendingRead();
 *   qb::io::ssl::create_server_context(const SSL_METHOD*, std::filesystem::path cert, std::filesystem::path key);
 *   ssl::socket: void set_insecure(); int connect_v4(std::string const&, uint16_t); bool handshake_complete() const;
 *                int handshake_status(); int read(void*, size_t); int write(const void*, size_t);
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
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/transport/saccept.h>
#include <qb/io/transport/stcp.h>

#include "../../shared/loopback_fixture.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_deadline.h"
#include "../../shared/tls_pump.h"

using namespace std::chrono_literals;

using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

SSL_CTX *
make_server_context() {
    return qb::io::ssl::create_server_context(TLS_server_method(), ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
}

// Drive a server-side TLS handshake to completion under a deadline. Mirrors the
// ssl-socket-loopback harness: handshake_status() returns 1 (done), 0 (in
// progress) or <0 (fatal).
void
drive_server_handshake(qb::io::tcp::ssl::socket &socket, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
        const int status = socket.handshake_status();
        if (status < 0) {
            ADD_FAILURE() << "server handshake_status reported a fatal error: " << status;
            socket.disconnect();
            return;
        }
        if (status == 1)
            break;
        std::this_thread::sleep_for(1ms);
    }
    if (!socket.handshake_complete()) {
        ADD_FAILURE() << "server handshake did not complete within the deadline";
        socket.disconnect();
    }
}

// Deadline-bounded encrypted read of exactly `n` bytes on a raw ssl::socket.
//
// Non-blocking first, and that is what makes `deadline` a deadline: the `else`
// arm below is `ssl::socket::read()` reporting WANT_READ/WANT_WRITE as 0
// (ssl::socket::read), which only ever happens on a NON-blocking socket.
// `connect_v4`/`accept` leave these sockets in the OS default (blocking), so the
// loop used to park inside SSL_read and re-check the clock only between reads it
// could not return from -- and every caller runs on a thread the other side joins.
bool
ssl_read_exactly(qb::io::tcp::ssl::socket &socket, char *out, std::size_t n, std::chrono::milliseconds timeout = 2s) {
    socket.set_nonblocking(true);
    std::size_t got      = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    while (got < n && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.read(out + got, n - got);
        if (ret > 0)
            got += static_cast<std::size_t>(ret);
        else if (ret < 0)
            return false;
        else
            std::this_thread::sleep_for(1ms);
    }
    return got == n;
}

// Deadline-bounded encrypted write of exactly `n` bytes on a raw ssl::socket.
// Same reasoning as ssl_read_exactly: `ssl::socket::write()` maps WANT_WRITE /
// WANT_READ to 0 (ssl::socket::write), so the retry arm needs the socket
// non-blocking to ever be taken. Read and write agree on the mode.
bool
ssl_write_exactly(qb::io::tcp::ssl::socket &socket, const char *in, std::size_t n, std::chrono::milliseconds timeout = 2s) {
    socket.set_nonblocking(true);
    std::size_t sent     = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    while (sent < n && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.write(in + sent, n - sent);
        if (ret > 0)
            sent += static_cast<std::size_t>(ret);
        else if (ret < 0)
            return false;
        else
            std::this_thread::sleep_for(1ms);
    }
    return sent == n;
}

} // namespace

// ===========================================================================
// Compile-time secure-ness
// ===========================================================================

TEST(SecureTransport, BothMixinsAreSecureAtCompileTime) {
    static_assert(qb::io::transport::saccept::is_secure());
    static_assert(qb::io::transport::stcp::is_secure());
    EXPECT_TRUE(qb::io::transport::saccept::is_secure());
    EXPECT_TRUE(qb::io::transport::stcp::is_secure());
}

// ===========================================================================
// saccept.read() with no pending connection
// ===========================================================================

TEST(SecureTransport, SacceptReadWithNoPendingConnectionReportsFailure) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::transport::saccept acceptor;
    acceptor.transport().init(make_server_context());
    ASSERT_NE(acceptor.transport().ssl_handle(), nullptr);
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), 0);
    acceptor.transport().set_nonblocking(true);

    // No client has connected: the secure accept cannot produce a socket.
    EXPECT_EQ(acceptor.read(), static_cast<std::size_t>(-1));

    acceptor.close();
    EXPECT_FALSE(acceptor.transport().is_open()) << "close() must shut the secure listener";
}

// ===========================================================================
// Full saccept(server) + stcp(client) TLS round-trip
// ===========================================================================

TEST(SecureTransport, SacceptAndStcpRoundTripEncryptedPayload) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::transport::saccept acceptor;
    acceptor.transport().init(make_server_context());
    ASSERT_NE(acceptor.transport().ssl_handle(), nullptr);
    ASSERT_TRUE(acceptor.transport().set_supported_alpn_protocols({"qb-test"}));
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = acceptor.transport().local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_ok{false};
    std::thread       server_thread([&] {
        // Bounded secure accept. With the listener non-blocking, saccept.read()
        // reports "nothing queued" as (size_t)-1 -- the contract
        // SacceptReadWithNoPendingConnectionReportsFailure above already pins -- so the
        // deadline is reachable. Blocking, it parked this thread in ::accept forever
        // whenever the client's connect never landed, and the main body below joins
        // this thread unconditionally.
        acceptor.transport().set_nonblocking(true);
        server_ready             = true;
        std::size_t handle       = static_cast<std::size_t>(-1);
        const auto  accept_until = std::chrono::steady_clock::now() + 5s;
        while ((handle = acceptor.read()) == static_cast<std::size_t>(-1) && std::chrono::steady_clock::now() < accept_until)
            std::this_thread::sleep_for(1ms);
        if (handle == static_cast<std::size_t>(-1)) {
            ADD_FAILURE() << "saccept.read() failed to accept the pending secure connection";
            return;
        }

        qb::io::tcp::ssl::socket &server_socket = acceptor.getAccepted();
        EXPECT_EQ(static_cast<std::size_t>(server_socket.native_handle()), handle) << "getAccepted() must wrap the handle read() returned";

        // Complete the server-side handshake, then echo one encrypted line.
        drive_server_handshake(server_socket);
        if (!server_socket.handshake_complete())
            return;

        char buffer[16] = {};
        if (!ssl_read_exactly(server_socket, buffer, 5))
            return;
        EXPECT_EQ(std::string_view(buffer, 5), "hello");
        if (!ssl_write_exactly(server_socket, "world", 5))
            return;
        server_ok = true;
        // The acceptor owns and closes server_socket on its destruction; the
        // moved-out flush() semantics are exercised in the dedicated test below.
    });

    // Join on every exit path: the ASSERTs below are fatal, and without this a failing
    // one returns from the body leaving `server_thread` joinable — std::terminate, and
    // the failure never reaches the report. Safe because the accept above is bounded.
    const qb::io::test::thread_joiner server_joiner{server_thread};

    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // Client side: a transport::stcp wrapping an ssl::socket.
    qb::io::transport::stcp client;
    client.transport().set_insecure(); // self-signed test cert
    ASSERT_TRUE(client.transport().set_sni_hostname("localhost"));
    ASSERT_TRUE(client.transport().set_alpn_protocols({"qb-test"}));
    // Exercise the synchronous API; its TLS phase is also bounded by the fixture.
    ASSERT_EQ(qb::io::test::connect_tls_blocking_until(client.transport(), port, std::chrono::steady_clock::now() + 5s), 0);
    ASSERT_TRUE(client.transport().handshake_complete());

    // Send "hello" through the stcp stream half (publish into _out_buffer, then
    // drain it with write()). Both poll loops below treat 0 as "would-block, retry",
    // which is only ever produced by a NON-blocking socket -- and `connect_v4` goes
    // through the plain blocking connect and never touches the flag, so the socket
    // arrives here blocking and neither 3s deadline could be reached. (The comment
    // this replaces claimed the socket "may be non-blocking after the handshake";
    // measured, it is not, and that wrong comment is what kept both loops looking
    // bounded.) Flip it once, here, for the write and the read that follow.
    ASSERT_EQ(client.transport().set_nonblocking(true), 0);
    ASSERT_NE(client.publish("hello", 5), nullptr);
    {
        const auto wdeadline = std::chrono::steady_clock::now() + 3s;
        while (client.pendingWrite() > 0 && std::chrono::steady_clock::now() < wdeadline) {
            const int wret = client.write();
            if (wret < 0) {
                FAIL() << "stcp.write() reported a fatal error: " << wret;
            }
            if (wret == 0)
                std::this_thread::sleep_for(1ms);
        }
        ASSERT_EQ(client.pendingWrite(), 0u) << "stcp.write() never flushed the encrypted payload";
    }

    // Drain the echoed "world" via stcp.read() (the SSL-pending-aware override).
    // stcp.read() returns >0 (bytes appended to in()), 0 (would-block / no data
    // yet), or <0 (fatal). Bounded poll until the 5 echoed bytes are buffered.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (client.pendingRead() < 5 && std::chrono::steady_clock::now() < deadline) {
        const int ret = client.read();
        if (ret < 0)
            break; // fatal stcp read error
        if (ret == 0)
            std::this_thread::sleep_for(1ms);
    }
    ASSERT_GE(client.pendingRead(), 5u) << "stcp.read() never drained the echoed reply";
    EXPECT_EQ(std::string_view(client.in().begin(), 5), "world");

    server_thread.join();
    EXPECT_TRUE(server_ok.load());

    client.transport().disconnect();
}

// saccept::flush mirrors production: protocol::accept::onMessage() has already
// std::move()d the accepted ssl::socket into a session, leaving getAccepted() in a
// moved-from (empty) state; flush() then just release_handle()s that empty slot.
// Here we accept a real secure connection, std::move() the socket out (taking the
// fd + SSL with it), and assert getAccepted() is already empty and flush() is a
// harmless no-op leaving it empty.
TEST(SecureTransport, SacceptFlushReleasesAlreadyMovedOutSlot) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::transport::saccept acceptor;
    acceptor.transport().init(make_server_context());
    ASSERT_NE(acceptor.transport().ssl_handle(), nullptr);
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), 0);
    const auto port = acceptor.transport().local_endpoint().port();
    ASSERT_NE(port, 0);

    // A client connects on a worker thread. connect_v4 is blocking and drives the
    // full TLS handshake, so the server side below must drive its half (on the
    // moved-out `owner`) to let the client's connect return.
    std::atomic<bool>        client_connected{false};
    qb::io::tcp::ssl::socket client;
    std::thread              client_thread([&] {
        client.set_insecure();
        // The result was discarded: a connect that failed left the main thread's
        // accept below with nothing to accept, and no record of why.
        const int result = qb::io::test::connect_tls_blocking_until(client, port, std::chrono::steady_clock::now() + 5s);
        EXPECT_EQ(result, 0) << "secure client connect failed";
        client_connected = result == 0 && client.handshake_complete();
    });

    const qb::io::test::thread_joiner client_joiner{client_thread};

    // Bounded, on the MAIN thread: a blocking accept here parks the thread that owns
    // every assertion below AND the client_thread.join() that would report the
    // client's own failure. Same non-blocking contract as the server thread above.
    acceptor.transport().set_nonblocking(true);
    std::size_t handle       = static_cast<std::size_t>(-1);
    const auto  accept_until = std::chrono::steady_clock::now() + 5s;
    while ((handle = acceptor.read()) == static_cast<std::size_t>(-1) && std::chrono::steady_clock::now() < accept_until)
        std::this_thread::sleep_for(1ms);
    ASSERT_NE(handle, static_cast<std::size_t>(-1)) << "saccept.read() failed to accept";

    // Move the accepted ssl::socket out (this is what onMessage() does): the slot
    // is emptied, taking the fd + SSL with it. `owner` now closes on scope exit.
    qb::io::tcp::ssl::socket owner = std::move(acceptor.getAccepted());
    EXPECT_TRUE(owner.is_open());
    EXPECT_EQ(acceptor.getAccepted().native_handle(), qb::io::inet::invalid_socket)
        << "after the move-out, the acceptor's slot must already be empty";

    // flush() on the moved-from slot is a no-op that keeps it empty (never a UAF /
    // double-close on the handle now owned by `owner`).
    acceptor.flush(0);
    EXPECT_EQ(acceptor.getAccepted().native_handle(), qb::io::inet::invalid_socket);

    // Drive the server-side handshake on the moved-out socket so the blocking
    // client connect_v4 can complete and the worker can join.
    drive_server_handshake(owner);

    client_thread.join();
    EXPECT_TRUE(client_connected.load());
    owner.disconnect();
    client.disconnect();
}

// ===========================================================================
// stcp under a read cap: the room left is read, the rest of a record stays inside the session (Huly QB-86)
// ===========================================================================
//
// Until 3.4 `stcp::read()` asked for a whole bucket (`QB_DEFAULT_READ_BUFFER_SIZE`, 64 KiB) or nothing: a cap below
// the bucket refused every read, and a stream within a bucket of its cap was refused although its octets fit. It now
// reads the room left. A TLS record is decrypted whole, so a read that takes less than the record leaves the rest
// INSIDE the session (`SSL_pending()`), where no readiness of the socket will ever announce it: the cases below pin
// that it stays there, intact, and is what the next read with room takes.

namespace secure_transport_loopback_test {

constexpr std::size_t kBucket    = QB_DEFAULT_READ_BUFFER_SIZE;
constexpr std::size_t kMaxRecord = 16384u; // the most plaintext one TLS record carries, and one `SSL_write()` here

/// `size` octets no two neighbours of which are equal, so an octet lost, repeated or out of place shows.
std::string
patterned(std::size_t size) {
    std::string payload(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        payload[i] = static_cast<char>('a' + (i % 23));
    return payload;
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

/// An established TLS connection, both ends non-blocking and driven by the test thread: the accepted end is the
/// socket of a `transport::stcp` -- the reader under test -- and `peer` writes to it, one record per write.
class StcpUnderACap {
    qb::io::tcp::ssl::listener _listener;

public:
    qb::io::tcp::ssl::socket peer;
    qb::io::transport::stcp  reader;

    [[nodiscard]] ::testing::AssertionResult
    open() {
        if (!require_ssl_files())
            return ::testing::AssertionFailure() << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");
        _listener.init(make_server_context());
        if (_listener.listen_v4(0, "127.0.0.1") != 0)
            return ::testing::AssertionFailure() << "listen_v4 failed";
        qb::io::test::tls_pair pair;
        pair.client.set_insecure(); // self-signed test cert
        if (auto opened = qb::io::test::open_tls_connection(_listener, _listener.local_endpoint().port(), pair); !opened)
            return opened;
        if (auto shaken = qb::io::test::complete_tls_handshake(pair); !shaken)
            return shaken;
        peer               = std::move(pair.client);
        reader.transport() = std::move(pair.server);
        return ::testing::AssertionSuccess();
    }

    /// The peer writes `octets` as ONE record: a write of at most `kMaxRecord` octets is one `SSL_write()`.
    [[nodiscard]] ::testing::AssertionResult
    send_record(std::string_view octets) {
        if (octets.empty() || octets.size() > kMaxRecord)
            return ::testing::AssertionFailure() << "not one record: " << octets.size() << " octets";
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            const int ret = peer.write(octets.data(), octets.size());
            if (ret == static_cast<int>(octets.size()))
                return ::testing::AssertionSuccess();
            if (ret != 0)
                return ::testing::AssertionFailure() << "the peer's write answered " << ret << ": " << peer.get_last_ssl_error_string();
            if (std::chrono::steady_clock::now() >= deadline)
                return ::testing::AssertionFailure() << "the peer's socket never took the record";
            std::this_thread::sleep_for(1ms);
        }
    }

    /// `reader.read()` once the record the peer wrote has crossed the loopback: 0 from a TLS read is "not yet" --
    /// a record is decrypted whole, so nothing of it is returned before its last octet arrived.
    [[nodiscard]] int
    read_when_it_arrived() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            const int ret = reader.read();
            if (ret != 0 || std::chrono::steady_clock::now() >= deadline)
                return ret;
            std::this_thread::sleep_for(1ms);
        }
    }

    /// Decrypted octets the reader's TLS session holds, as OpenSSL counts them.
    [[nodiscard]] std::size_t
    held_in_session() {
        return static_cast<std::size_t>(SSL_pending(reader.transport().ssl_handle()));
    }

    ~StcpUnderACap() {
        static_cast<void>(peer.disconnect());
        static_cast<void>(reader.transport().disconnect());
    }
};

} // namespace secure_transport_loopback_test

// A cap below the read bucket reads up to it, and is refused only once no room is left; what the record holds past
// the cap stays inside the TLS session -- the socket has nothing more to give -- and is read, whole and in order, as
// the buffer is flushed.
TEST(SecureTransport, StcpUnderAReadCapBelowTheBucketReadsUpToItAndKeepsTheRestOfTheRecord) {
    using namespace secure_transport_loopback_test;
    constexpr std::size_t kCap = 1024u;
    ASSERT_LT(kCap, kBucket);
    const std::string payload = patterned(3000u);

    StcpUnderACap tls;
    ASSERT_TRUE(tls.open());
    tls.reader.set_max_read_buffer_size(kCap);
    ASSERT_TRUE(tls.send_record(payload));

    ASSERT_EQ(tls.read_when_it_arrived(), static_cast<int>(kCap)) << "a cap below the bucket reads up to the cap";
    EXPECT_EQ(tls.reader.pendingRead(), kCap);
    EXPECT_EQ(tls.held_in_session(), payload.size() - kCap) << "the rest of the record is held inside the TLS session";
    EXPECT_EQ(waiting_in_socket(tls.reader.transport()), std::optional<std::size_t>{0u})
        << "and the socket has nothing left: no readiness will announce what the session holds";

    EXPECT_EQ(tls.reader.read(), qb::io::ErrBufferLimitExceeded) << "no room left: refused";
    EXPECT_EQ(tls.reader.pendingRead(), kCap) << "nothing read past the cap";
    EXPECT_EQ(tls.held_in_session(), payload.size() - kCap) << "a refused read drops nothing of what the session holds";

    std::string assembled;
    for (std::size_t step = 0; assembled.size() < payload.size() && step < 64u; ++step) {
        ASSERT_LE(tls.reader.pendingRead(), kCap) << "the buffer never holds more than its cap";
        const std::size_t taken = tls.reader.pendingRead() < 600u ? tls.reader.pendingRead() : 600u; // a protocol consumes
        assembled.append(tls.reader.in().begin(), taken);
        tls.reader.flush(taken);
        tls.reader.eof();
        const std::size_t room = kCap - tls.reader.pendingRead();
        const std::size_t held = tls.held_in_session();
        if (held == 0u)
            continue; // the record is read: what is buffered is all there is
        ASSERT_EQ(tls.reader.read(), static_cast<int>(room < held ? room : held))
            << "what the session holds is read as far as the room goes, with nothing more from the socket";
    }
    EXPECT_EQ(assembled, payload) << "every octet of the record, once, in order";
    EXPECT_EQ(tls.held_in_session(), 0u);
}

// A stream that fills exactly to its cap is read whole, and one more octet is refused. The cap is a bucket and 1000
// octets: once the buffer holds more than 1000 octets the room left is less than a bucket, which is where the read was
// refused although the rest of the stream fits.
TEST(SecureTransport, StcpReadsAStreamThatFillsExactlyToItsCapAndRefusesOneMoreOctet) {
    using namespace secure_transport_loopback_test;
    constexpr std::size_t kCap    = kBucket + 1000u;
    const std::string     payload = patterned(kCap + 1u); // the stream, and one octet more

    StcpUnderACap tls;
    ASSERT_TRUE(tls.open());
    tls.reader.set_max_read_buffer_size(kCap);

    std::size_t sent = 0;
    while (sent < kCap) { // record by record: four of 16384 octets, then the 1000 that fill the cap
        const std::size_t record = kCap - sent < kMaxRecord ? kCap - sent : kMaxRecord;
        ASSERT_TRUE(tls.send_record(std::string_view{payload}.substr(sent, record)));
        ASSERT_EQ(tls.read_when_it_arrived(), static_cast<int>(record))
            << "refused with " << tls.reader.pendingRead() << " octets buffered under a cap of " << kCap;
        sent += record;
    }
    ASSERT_EQ(tls.reader.pendingRead(), kCap) << "the stream that fits its cap is read whole";
    EXPECT_EQ(std::string_view(tls.reader.in().begin(), tls.reader.pendingRead()), std::string_view{payload}.substr(0u, kCap));
    EXPECT_EQ(tls.held_in_session(), 0u);

    // One octet more: it reaches the socket, and the read is refused without taking it.
    ASSERT_TRUE(tls.send_record(std::string_view{payload}.substr(kCap, 1u)));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (waiting_in_socket(tls.reader.transport()).value_or(0u) == 0u && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_GT(waiting_in_socket(tls.reader.transport()).value_or(0u), 0u) << "the record of the extra octet never arrived";
    EXPECT_EQ(tls.reader.read(), qb::io::ErrBufferLimitExceeded) << "one more octet does not fit: refused";
    EXPECT_EQ(tls.reader.pendingRead(), kCap) << "nothing read past the cap, nothing dropped";

    tls.reader.flush(1u);
    tls.reader.eof();
    ASSERT_EQ(tls.read_when_it_arrived(), 1) << "the refused octet was left for a read with room";
    EXPECT_EQ(tls.reader.in().begin()[tls.reader.pendingRead() - 1u], payload.back());
}

// The bucket bounds a read whatever the room, over TLS as over TCP: under a cap that leaves far more room than a
// bucket, a read reserves a bucket for the record it takes -- not the room left, which would be the whole cap.
TEST(SecureTransport, StcpReservesABucketForAReadWhateverTheRoom) {
    using namespace secure_transport_loopback_test;
    constexpr std::size_t kCap    = kBucket * 64u; // 4 MiB of room
    const std::string     payload = patterned(3000u);

    StcpUnderACap tls;
    ASSERT_TRUE(tls.open());
    tls.reader.set_max_read_buffer_size(kCap);
    ASSERT_TRUE(tls.send_record(payload));

    ASSERT_EQ(tls.read_when_it_arrived(), static_cast<int>(payload.size()));
    EXPECT_EQ(std::string_view(tls.reader.in().begin(), tls.reader.pendingRead()), std::string_view{payload});
    EXPECT_LT(tls.reader.in().capacity(), kCap) << "a read reserved the room left under the cap, not a bucket";
}

// `last_read_filled()` is what the io layer tests after a read, before it asks what the session holds: only a read
// that filled the place it asked for can have left octets of its record there. It is a "may", and `held_input()` the
// count: a record that ends exactly where the room does fills its read and leaves nothing.
TEST(SecureTransport, StcpRecordsWhetherItsLastReadFilledThePlaceItAskedFor) {
    using namespace secure_transport_loopback_test;
    constexpr std::size_t kCap    = 1024u;
    const std::string     payload = patterned(3000u);

    StcpUnderACap tls;
    ASSERT_TRUE(tls.open());
    tls.reader.set_max_read_buffer_size(kCap);
    EXPECT_FALSE(tls.reader.last_read_filled()) << "no read was made";
    ASSERT_EQ(tls.reader.read(), 0) << "nothing was sent";
    EXPECT_FALSE(tls.reader.last_read_filled()) << "a read that took nothing filled nothing";

    ASSERT_TRUE(tls.send_record(payload));
    ASSERT_EQ(tls.read_when_it_arrived(), static_cast<int>(kCap));
    EXPECT_TRUE(tls.reader.last_read_filled()) << "the read took all it asked for: its record may hold more";
    EXPECT_EQ(tls.reader.held_input(), payload.size() - kCap) << "and does";

    ASSERT_EQ(tls.reader.read(), qb::io::ErrBufferLimitExceeded);
    EXPECT_TRUE(tls.reader.last_read_filled()) << "a refused read reads nothing: what the session holds is still there to ask for";
    EXPECT_EQ(tls.reader.held_input(), payload.size() - kCap);

    tls.reader.flush(kCap);
    tls.reader.eof();
    ASSERT_EQ(tls.reader.read(), static_cast<int>(kCap));
    EXPECT_TRUE(tls.reader.last_read_filled());
    tls.reader.flush(kCap);
    tls.reader.eof();
    ASSERT_EQ(tls.reader.read(), static_cast<int>(payload.size() - 2u * kCap)) << "the end of the record: less than the room";
    EXPECT_FALSE(tls.reader.last_read_filled()) << "a read that took less than it asked for took the end of its record";
    EXPECT_EQ(tls.reader.held_input(), 0u);

    tls.reader.flush(tls.reader.pendingRead());
    tls.reader.eof();
    ASSERT_TRUE(tls.send_record(std::string_view{payload}.substr(0u, kCap))); // a record of exactly the room
    ASSERT_EQ(tls.read_when_it_arrived(), static_cast<int>(kCap));
    EXPECT_TRUE(tls.reader.last_read_filled()) << "a read its record fills exactly is one that may have left octets";
    EXPECT_EQ(tls.reader.held_input(), 0u) << "and held_input() says it left none";
}
