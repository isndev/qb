/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/ssl-context-handshake.cpp
 * @brief End-to-end loopback TLS handshakes driven entirely by the value-semantic `ssl::Context` API.
 *
 * The capstone for the `qb::io::ssl::Context` abstraction: a real OpenSSL handshake where BOTH sides
 * are built from a `Context` — server = `listener{Context::server(cert,key).alpn(...)}`, client =
 * `socket{Context::client()...}`. It proves, at the socket level:
 *   - the Context client path: secure-by-default verification, the per-connection `insecure()` override,
 *     and ALPN offered from the context;
 *   - the Context server path: accept + ALPN selection driven by the context's ex-data wire buffer;
 *   - that a VERIFYING client Context REJECTS the self-signed test server — the context's verify mode is
 *     respected, never force-overridden by the socket.
 *
 * POSIX-only busy-poll harness (WINDOWS_EXCLUDE), mirroring system/tcp/ssl-socket-loopback.cpp: both
 * sides drive a blocking/near-blocking handshake, so on Windows the client can complete + close before
 * the server's poll observes it. The event-loop SSL path is covered on Windows by qbm-http.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/context.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"

using namespace std::chrono_literals;
using qb::io::ssl::Context;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

// Drive the server-side TLS handshake to completion (or a bounded failure) under a wall-clock deadline.
void
drive_server_handshake(qb::io::tcp::ssl::socket &socket, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
        const int status = socket.handshake_status();
        if (status < 0)
            return; // fatal (expected in the reject case) — stop driving
        if (status == 1)
            return;
        std::this_thread::sleep_for(1ms);
    }
}

} // namespace

// ===========================================================================
// Both sides from a Context: full handshake + ALPN negotiated on both ends.
// ===========================================================================
TEST(SslContextHandshake, ContextClientAndServerNegotiateAlpn) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener{
        Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2", "http/1.1"})};
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::string       server_alpn;
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        if (listener.accept(server_socket) != 0)
            return;
        drive_server_handshake(server_socket);
        if (server_socket.handshake_complete())
            server_alpn = server_socket.get_alpn_selected_protocol();
    });

    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // Client built from a Context; self-signed test cert -> opt this connection out of verification.
    qb::io::tcp::ssl::socket client{Context::client().alpn({"h2", "http/1.1"})};
    client.insecure().sni("localhost");
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    EXPECT_TRUE(client.handshake_complete());
    EXPECT_EQ(client.get_alpn_selected_protocol(), "h2");
    EXPECT_FALSE(client.get_negotiated_tls_version().empty());
    EXPECT_TRUE(client.context().ok());

    listener.disconnect();
    server_thread.join();
    EXPECT_EQ(server_alpn, "h2") << "server must select h2 via the context's ex-data ALPN callback";
}

// ===========================================================================
// A verifying client Context must REJECT the self-signed server (mode respected).
// ===========================================================================
TEST(SslContextHandshake, VerifyingContextRejectsSelfSignedServer) {
    ASSERT_TRUE(require_ssl_files());

    // Exercise the init(Context) path (what the qbm HTTP servers use), distinct from the ctor above.
    qb::io::tcp::ssl::listener listener;
    listener.init(Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        if (listener.accept(server_socket) != 0)
            return;
        drive_server_handshake(server_socket, 3s); // will fault on the client's verify abort — bounded
    });

    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // Secure-by-default Context (verify = peer, system trust store) with NO insecure() override: the
    // self-signed test cert is untrusted, so the handshake must abort.
    qb::io::tcp::ssl::socket client{Context::client()};
    client.sni("localhost");
    const int rc = client.connect_v4("127.0.0.1", port);
    EXPECT_NE(rc, 0) << "a verifying Context must not report a successful connect to a self-signed server";
    EXPECT_FALSE(client.handshake_complete()) << "a verifying Context must reject the self-signed server";

    listener.disconnect();
    server_thread.join();
}

// ===========================================================================
// Typed client callbacks (on_keylog + on_verify) fire during the handshake.
// Proves Correction B: the std::functions on the SSL_CTX ex-data are reachable
// from the minted SSL and drive real handshake behavior.
// ===========================================================================
TEST(SslContextHandshake, TypedClientCallbacksFireDuringHandshake) {
    ASSERT_TRUE(require_ssl_files());

    qb::io::tcp::ssl::listener listener{
        Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2"})};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        if (listener.accept(server_socket) != 0)
            return;
        drive_server_handshake(server_socket);
    });
    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    std::atomic<int> keylog_lines{0};
    std::atomic<int> verify_calls{0};
    // NOT insecure(): the context verifies, so on_verify runs. It accepts everything (returns true), so the
    // self-signed server is accepted THROUGH the callback — proving on_verify both fires and governs.
    auto ctx = Context::client()
                   .alpn({"h2"})
                   .on_keylog([&](std::string_view line) {
                       if (!line.empty())
                           keylog_lines.fetch_add(1);
                   })
                   .on_verify([&](bool /*preverified*/, qb::io::ssl::VerifyContext &vc) {
                       verify_calls.fetch_add(1);
                       (void) vc.depth();
                       (void) vc.error();
                       (void) vc.error_string();
                       (void) vc.current_certificate();
                       return true; // accept despite the self-signed chain
                   });
    qb::io::tcp::ssl::socket client{std::move(ctx)};
    client.sni("localhost");
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    EXPECT_TRUE(client.handshake_complete()) << "on_verify returning true must let the handshake complete";
    EXPECT_GT(verify_calls.load(), 0) << "on_verify must fire during chain verification";
    EXPECT_GT(keylog_lines.load(), 0) << "on_keylog must fire with TLS key material";

    listener.disconnect();
    server_thread.join();
}

// ===========================================================================
// Server on_sni callback fires with the client's SNI hostname.
// ===========================================================================
TEST(SslContextHandshake, ServerSniCallbackFiresWithClientHostname) {
    ASSERT_TRUE(require_ssl_files());

    std::atomic<int> sni_calls{0};
    std::string      seen_host; // written on the server thread, read after join (happens-before)
    auto             server_ctx = Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"))
                          .on_sni([&](std::string_view host) {
                              sni_calls.fetch_add(1);
                              seen_host = std::string(host);
                              return Context{}; // empty -> keep the current context
                          });
    qb::io::tcp::ssl::listener listener{std::move(server_ctx)};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        if (listener.accept(server_socket) != 0)
            return;
        drive_server_handshake(server_socket);
    });
    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // connect_v4 sends its host argument as the client's SNI (it overrides any earlier sni()), so the
    // server's on_sni must observe exactly that.
    qb::io::tcp::ssl::socket client{Context::client()};
    client.insecure();
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    EXPECT_TRUE(client.handshake_complete());

    listener.disconnect();
    server_thread.join();
    EXPECT_GT(sni_calls.load(), 0) << "the server on_sni callback must fire";
    EXPECT_EQ(seen_host, "127.0.0.1") << "on_sni must receive the SNI the client sent (the connect host)";
}

// ===========================================================================
// A per-connection socket.alpn() override wins over the context's ALPN list.
// ===========================================================================
TEST(SslContextHandshake, PerConnectionAlpnOverridesContext) {
    ASSERT_TRUE(require_ssl_files());

    qb::io::tcp::ssl::listener listener{
        Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")).alpn({"h2", "http/1.1"})};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::string       server_alpn;
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        if (listener.accept(server_socket) != 0)
            return;
        drive_server_handshake(server_socket);
        if (server_socket.handshake_complete())
            server_alpn = server_socket.get_alpn_selected_protocol();
    });
    while (!server_ready.load())
        std::this_thread::sleep_for(1ms);

    // The context offers only http/1.1; the per-connection override offers h2 -> h2 must be negotiated.
    qb::io::tcp::ssl::socket client{Context::client().alpn({"http/1.1"})};
    client.insecure().alpn({"h2"});
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    EXPECT_TRUE(client.handshake_complete());
    EXPECT_EQ(client.get_alpn_selected_protocol(), "h2") << "the per-connection alpn() override must win over the context's";

    listener.disconnect();
    server_thread.join();
    EXPECT_EQ(server_alpn, "h2");
}

// ===========================================================================
// A2 fix: on a Context socket the SSL is minted lazily at connect, so
// set_session()/resume() BEFORE connect must DEFER the session (accept it) and
// apply it after SSL_new — not silently drop it (pre-A2, set_session returned
// false when _ssl_handle was null, so the saved session vanished and the
// handshake was a full one). Full server-side resumption (SSL_session_reused)
// is not asserted: it is unreliable in a minimal loopback, which is why the
// sibling ssl-socket-loopback resumption test also only checks the session is
// accepted, not reused.
// ===========================================================================
TEST(SslContextHandshake, SessionResumeBeforeConnectIsDeferredNotDropped) {
    ASSERT_TRUE(require_ssl_files());

    qb::io::tcp::ssl::listener listener{Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"))};
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<int> accepted{0};
    // Accept exactly the two connections this test makes, then self-exit. A `while(!stop)` loop
    // would re-enter a BLOCKING accept() after the 2nd connection; on macOS/BSD a cross-thread
    // listener.disconnect()/shutdown() does NOT wake a blocked accept() (ENOTCONN on a listening
    // socket), so join() would hang — a race the fast dev build won and ASan/TSan timing lost.
    // Bounding the loop keeps teardown portable and race-free, mirroring the single-accept
    // sibling tests above (the server thread self-exits; no cross-thread accept cancellation).
    std::thread server_thread([&] {
        for (int i = 0; i < 2; ++i) {
            qb::io::tcp::ssl::socket server_socket;
            if (listener.accept(server_socket) != 0)
                return; // real error — stop driving
            accepted.fetch_add(1);
            drive_server_handshake(server_socket);
        }
    });

    // First connection captures a session.
    qb::io::ssl::Session session{};
    {
        qb::io::tcp::ssl::socket c1{Context::client()};
        c1.insecure();
        ASSERT_EQ(c1.connect_v4("127.0.0.1", port), 0);
        ASSERT_TRUE(c1.handshake_complete());
        session = c1.get_session();
    }
    ASSERT_TRUE(session.is_valid()) << "the first handshake must yield a session";

    // Second connection: set_session()/resume() BEFORE connect on a Context socket (SSL not minted yet).
    {
        qb::io::tcp::ssl::socket c2{Context::client()};
        EXPECT_TRUE(c2.set_session(session)) << "set_session/resume before connect must be DEFERRED (true), not dropped (false)";
        c2.insecure();
        ASSERT_EQ(c2.connect_v4("127.0.0.1", port), 0);
        EXPECT_TRUE(c2.handshake_complete()) << "the deferred session must be applied without breaking the handshake";
    }

    server_thread.join(); // exits after exactly two accepts — no cross-thread accept cancellation
    qb::io::ssl::free_session(session);
    listener.disconnect();
    EXPECT_GE(accepted.load(), 2);
}

// No in-file main(): links the framework's shared gtest-main.
