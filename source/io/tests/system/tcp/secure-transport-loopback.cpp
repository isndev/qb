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
 * POSIX-only (`WINDOWS_EXCLUDE`): like ssl-socket-loopback.cpp, both sides busy-poll the non-blocking
 * handshake; on Windows the completion ordering races (the production async event-loop SSL path is
 * Windows-clean — see qbm-http). SSL is gated at CMake (`REQUIRES ssl`); the shipped self-signed
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
#include <cstring>
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

#include "../../shared/ssl_fixtures.h"

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
            return;
        }
        if (status == 1)
            break;
        std::this_thread::sleep_for(1ms);
    }
    if (!socket.handshake_complete())
        ADD_FAILURE() << "server handshake did not complete within the deadline";
}

// Deadline-bounded encrypted read of exactly `n` bytes on a raw ssl::socket.
bool
ssl_read_exactly(qb::io::tcp::ssl::socket &socket, char *out, std::size_t n, std::chrono::milliseconds timeout = 2s) {
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
bool
ssl_write_exactly(qb::io::tcp::ssl::socket &socket, const char *in, std::size_t n, std::chrono::milliseconds timeout = 2s) {
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
        server_ready = true;
        // saccept.read() accepts the pending secure connection and returns the
        // accepted native handle (never (size_t)-1 here).
        const std::size_t handle = acceptor.read();
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

    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // Client side: a transport::stcp wrapping an ssl::socket.
    qb::io::transport::stcp client;
    client.transport().set_insecure(); // self-signed test cert
    ASSERT_TRUE(client.transport().set_sni_hostname("localhost"));
    ASSERT_TRUE(client.transport().set_alpn_protocols({"qb-test"}));
    // Blocking connect_v4 completes both the TCP connect and the TLS handshake.
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.transport().handshake_complete());

    // Send "hello" through the stcp stream half (publish into _out_buffer, then
    // drain it with write()). The socket may be non-blocking after the handshake,
    // so a single write() can report 0 (WANT_WRITE) — loop until the buffer empties.
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
        client.connect_v4("127.0.0.1", port);
        client_connected = true;
    });

    const std::size_t handle = acceptor.read();
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
