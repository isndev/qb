/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/ssl-socket-loopback.cpp
 * @brief Real loopback TLS handshakes over `qb::io::tcp::ssl::{socket,listener}`.
 *
 * The system-tier half of the former test-ssl-socket.cpp: every case here opens real loopback
 * sockets and performs a genuine OpenSSL handshake (TLS 1.2/1.3) over IPv4 / IPv6 / AF_UNIX. It
 * proves the negotiated state a completed handshake exposes (cipher suite, TLS version, ALPN, the
 * full peer-certificate chain + fields, session resumption), the blocking + timeout connect-variant
 * matrix, the no-context accept failure path, and the non-blocking `n_connect*` family. The pure
 * SSL_CTX / SSL-object configuration logic that needs no socket lives in the sibling unit test
 * unit/ssl/ssl-context-config.cpp.
 *
 * Hardening over the original (per the restructure spec §2/§7):
 *   - cert presence is a HARD prerequisite (`ASSERT_TRUE(require_ssl_files())`), not a `GTEST_SKIP`
 *     — the harness ships the cert, so its absence is an environment bug, not a reason to vanish the
 *     positive matrix. The cert locator is the shared `tests/shared/ssl_fixtures.h`.
 *   - `drive_server_handshake` is de-flaked: instead of a fixed 200×1ms busy-loop that silently gives
 *     up, it is a single deadline-bounded poll that fails LOUDLY (`ADD_FAILURE`) on timeout and never
 *     hangs the runner; the bare `sleep_for(50ms)` keep-alive is gone (the server thread is held open
 *     by the join guard until the client is done).
 *   - `DriveNonBlockingConnectToCompletion` drives one non-blocking variant all the way to a
 *     completed handshake (poll `handshake_status()` to 1) — the non-blocking *success* leg is now
 *     positively verified, not just its in-progress leg.
 *   - `SecureByDefaultHandshakeRejectsSelfSignedServer` adds the missing negative path: a client that
 *     leaves `verify_peer()` ON must FAIL the handshake against the self-signed test cert, while a
 *     `set_insecure()` client succeeds — the secure-by-default rejection, proven at the socket level.
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
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"

using namespace std::chrono_literals;

using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

// IPv6-loopback availability: bind `::1` once (the v6 leg skips cleanly on hosts
// without IPv6 loopback rather than failing on listen).
bool
ipv6_loopback_available() {
    qb::io::tcp::ssl::listener probe;
    return probe.listen_v6(0, "::1") == 0;
}

// Drive the server-side TLS handshake to completion under a wall-clock deadline.
// De-flaked: a timeout is a LOUD failure naming the stall, never a silent give-up
// (the original 200×1ms loop simply stopped trying and left the client hanging).
void
drive_server_handshake(qb::io::tcp::ssl::socket &socket, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
        const int status = socket.handshake_status();
        if (status < 0) {
            ADD_FAILURE() << "server handshake_status reported a fatal error: " << status;
            return;
        }
        if (status == 1) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    if (!socket.handshake_complete()) {
        ADD_FAILURE() << "server handshake did not complete within the deadline";
    }
}

// RAII: ensure the server thread is always joined; `before_join` (typically a
// listener.disconnect()) unwedges a server still blocked in accept().
class thread_join_guard {
    std::thread          &_thread;
    std::function<void()> _before_join;

public:
    template <typename Fn>
    thread_join_guard(std::thread &thread, Fn &&before_join)
        : _thread(thread)
        , _before_join(std::forward<Fn>(before_join)) {}

    thread_join_guard(const thread_join_guard &)            = delete;
    thread_join_guard &operator=(const thread_join_guard &) = delete;

    ~thread_join_guard() {
        if (_thread.joinable()) {
            if (_before_join) {
                _before_join();
            }
            _thread.join();
        }
    }
};

// Deadline-bounded exact read/write over a (blocking) TLS socket. Returns a gtest
// AssertionResult so a stall fails with a precise byte-count message.
::testing::AssertionResult
read_exactly(qb::io::tcp::ssl::socket &socket, void *data, std::size_t size, std::chrono::milliseconds timeout = 2s) {
    auto       *out      = static_cast<char *>(data);
    std::size_t received = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    int         last     = 0;

    while (received < size && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.read(out + received, size - received);
        last          = ret;
        if (ret > 0) {
            received += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret < 0) {
            return ::testing::AssertionFailure() << "SSL read failed after " << received << "/" << size << " bytes";
        }
        std::this_thread::sleep_for(1ms);
    }

    if (received == size) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "timed out waiting for " << size << " SSL bytes; received " << received
                                         << ", last read result=" << last;
}

::testing::AssertionResult
write_exactly(qb::io::tcp::ssl::socket &socket, const void *data, std::size_t size, std::chrono::milliseconds timeout = 2s) {
    const auto *in       = static_cast<const char *>(data);
    std::size_t written  = 0;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    int         last     = 0;

    while (written < size && std::chrono::steady_clock::now() < deadline) {
        const int ret = socket.write(in + written, size - written);
        last          = ret;
        if (ret > 0) {
            written += static_cast<std::size_t>(ret);
            continue;
        }
        if (ret < 0) {
            return ::testing::AssertionFailure() << "SSL write failed after " << written << "/" << size << " bytes";
        }
        std::this_thread::sleep_for(1ms);
    }

    if (written == size) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "timed out writing " << size << " SSL bytes; wrote " << written << ", last write result=" << last;
}

bool
record_thread_failure(::testing::AssertionResult result) {
    if (result) {
        return true;
    }
    ADD_FAILURE() << result.message();
    return false;
}

// A configured self-signed server context built from the shipped test cert.
SSL_CTX *
make_server_context() {
    return qb::io::ssl::create_server_context(TLS_server_method(), ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
}

} // namespace

// ===========================================================================
// No-context accept fails cleanly after a real TCP accept
// ===========================================================================

TEST(SSLSocketLoopback, AcceptWithoutContextFailsCleanlyAfterTcpAccept) {
    {
        qb::io::tcp::ssl::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread client_thread([port] {
            qb::io::tcp::socket client;
            ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
            client.disconnect();
        });

        auto accepted = listener.accept();
        EXPECT_FALSE(accepted.is_open());
        EXPECT_EQ(accepted.ssl_handle(), nullptr);
        client_thread.join();
    }

    {
        qb::io::tcp::ssl::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread client_thread([port] {
            qb::io::tcp::socket client;
            ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
            client.disconnect();
        });

        qb::io::tcp::ssl::socket accepted;
        EXPECT_EQ(listener.accept(accepted), -1);
        EXPECT_FALSE(accepted.is_open());
        EXPECT_EQ(accepted.ssl_handle(), nullptr);
        client_thread.join();
    }
}

// ===========================================================================
// Flagship: a completed handshake exposes full negotiated state
// ===========================================================================

TEST(SSLSocketLoopback, LoopbackHandshakeExposesNegotiatedState) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_TRUE(listener.set_supported_alpn_protocols({"h2", "http/1.1"}));
    ASSERT_EQ(listener.listen_v4(0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_ok{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        EXPECT_FALSE(server_socket.get_negotiated_cipher_suite().empty());
        EXPECT_FALSE(server_socket.get_negotiated_tls_version().empty());
        EXPECT_EQ(server_socket.get_alpn_selected_protocol(), "h2");
        EXPECT_TRUE(server_socket.get_peer_certificate_details().subject.empty());

        char buffer[64] = {};
        if (!record_thread_failure(read_exactly(server_socket, buffer, 4))) {
            return;
        }
        EXPECT_EQ(std::string_view(buffer, 4), "ping");
        if (!record_thread_failure(write_exactly(server_socket, "pong", 4))) {
            return;
        }
        server_ok = true;
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    ASSERT_TRUE(client.set_sni_hostname("localhost"));
    ASSERT_TRUE(client.set_alpn_protocols({"h2", "http/1.1"}));
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());
    EXPECT_FALSE(client.get_negotiated_cipher_suite().empty());
    EXPECT_FALSE(client.get_negotiated_tls_version().empty());
    EXPECT_EQ(client.get_alpn_selected_protocol(), "h2");

    const auto certificate = client.get_peer_certificate_details();
    EXPECT_FALSE(certificate.subject.empty());
    EXPECT_FALSE(certificate.issuer.empty());
    EXPECT_FALSE(certificate.serial_number.empty());
    EXPECT_GT(certificate.not_after, certificate.not_before);
    EXPECT_FALSE(certificate.signature_algorithm.empty());

    const auto chain = client.get_peer_certificate_chain();
    EXPECT_FALSE(chain.empty());

    auto session = client.get_session();
    EXPECT_TRUE(session.is_valid());
    {
        qb::io::tcp::ssl::socket resumption_socket;
        auto                    *ctx = qb::io::ssl::create_client_context(TLS_client_method());
        ASSERT_NE(ctx, nullptr);
        resumption_socket.init(SSL_new(ctx));
        EXPECT_TRUE(resumption_socket.set_session(session));
    }
    qb::io::ssl::free_session(session);
    EXPECT_FALSE(session.is_valid());

    ASSERT_TRUE(write_exactly(client, "ping", 4));
    char reply[64] = {};
    ASSERT_TRUE(read_exactly(client, reply, 4));
    EXPECT_EQ(std::string_view(reply, 4), "pong");
    EXPECT_FALSE(client.request_client_post_handshake_auth());

    EXPECT_TRUE(server_ok.load());
}

// ===========================================================================
// Secure-by-default: a verifying client REJECTS the self-signed test cert
// ===========================================================================

TEST(SSLSocketLoopback, SecureByDefaultHandshakeRejectsSelfSignedServer) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // Accept up to two connections (verifying client, then insecure client) and
    // best-effort drive each handshake; the verifying client's handshake is
    // expected to be torn down by the client's verification failure.
    std::atomic<bool> stop{false};
    std::thread       acceptor([&] {
        for (int i = 0; i < 2 && !stop.load(); ++i) {
            qb::io::tcp::ssl::socket server_socket;
            if (listener.accept(server_socket) != 0) {
                continue;
            }
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!server_socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
                if (server_socket.handshake_status() < 0) {
                    break; // peer aborted (the verifying client rejecting our cert)
                }
                std::this_thread::sleep_for(1ms);
            }
        }
    });
    thread_join_guard acceptor_join(acceptor, [&] {
        stop = true;
        listener.disconnect();
    });

    // 1) A verifying (secure-by-default) client MUST fail against the self-signed cert.
    {
        qb::io::tcp::ssl::socket verifying_client;
        ASSERT_TRUE(verifying_client.verify_peer()) << "client must be verifying by default";
        ASSERT_TRUE(verifying_client.set_sni_hostname("localhost"));
        const int ret = verifying_client.connect_v4("localhost", port);
        EXPECT_NE(ret, 0) << "secure-by-default client accepted a self-signed certificate (MITM hole)";
        EXPECT_FALSE(verifying_client.handshake_complete());
    }

    // 2) A set_insecure() client MUST connect to the very same server.
    {
        qb::io::tcp::ssl::socket insecure_client;
        insecure_client.set_insecure();
        const int ret = insecure_client.connect_v4("127.0.0.1", port);
        EXPECT_EQ(ret, 0) << "set_insecure() failed to opt out of verification";
        EXPECT_TRUE(insecure_client.handshake_complete());
        insecure_client.disconnect();
    }
}

// ===========================================================================
// Blocking + timeout connect-variant matrix
// ===========================================================================

TEST(SSLSocketLoopback, BlockingUriAndEndpointTimeoutConnectVariantsReachLoopbackServer) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    constexpr int expected_connections = 3;

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<int>  server_count{0};
    std::thread       server_thread([&] {
        server_ready = true;
        for (int i = 0; i < expected_connections; ++i) {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);

            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            const char reply = static_cast<char>(marker + 1);
            if (!record_thread_failure(write_exactly(server_socket, &reply, sizeof(reply)))) {
                return;
            }
            ++server_count;
        }
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    auto exchange_marker = [](qb::io::tcp::ssl::socket &client, char marker) {
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, &marker, sizeof(marker)));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, static_cast<char>(marker + 1));
        client.disconnect();
    };

    qb::io::tcp::ssl::socket uri_client;
    uri_client.set_insecure();
    ASSERT_EQ(uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port))), 0);
    exchange_marker(uri_client, 'a');

    qb::io::tcp::ssl::socket timed_uri_client;
    timed_uri_client.set_insecure();
    ASSERT_EQ(timed_uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port)), 1s), 0);
    exchange_marker(timed_uri_client, 'b');

    qb::io::tcp::ssl::socket endpoint_timeout_client;
    endpoint_timeout_client.set_insecure();
    ASSERT_EQ(endpoint_timeout_client.connect(qb::io::endpoint("127.0.0.1", port), "localhost", 1s), 0);
    exchange_marker(endpoint_timeout_client, 'c');

    EXPECT_EQ(server_count.load(), expected_connections);
}

// ===========================================================================
// IPv6 + Unix connect variants complete a handshake
// ===========================================================================

TEST(SSLSocketLoopback, IPv6AndUnixConnectVariantsCompleteHandshake) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    if (ipv6_loopback_available()) {
        qb::io::tcp::ssl::listener listener;
        listener.init(make_server_context());
        ASSERT_NE(listener.ssl_handle(), nullptr);
        ASSERT_EQ(listener.listen_v6(0, "::1"), 0);
        const auto port = listener.local_endpoint().port();
        ASSERT_NE(port, 0);

        std::thread       server_thread([&] {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);
            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            EXPECT_EQ(marker, 'v');
            if (!record_thread_failure(write_exactly(server_socket, "6", 1))) {
                return;
            }
        });
        thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

        qb::io::tcp::ssl::socket client;
        client.set_insecure();
        ASSERT_EQ(client.connect_v6("::1", port), 0);
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, "v", 1));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, '6');
        client.disconnect();
    } else {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

#ifndef _WIN32
    {
        const auto path = std::string("/tmp/qb-ssl-socket-uri-") + std::to_string(::getpid()) + ".sock";
        const auto uri  = qb::io::uri("unix://" + path);
        std::remove(path.c_str());

        qb::io::tcp::ssl::listener listener;
        listener.init(make_server_context());
        ASSERT_NE(listener.ssl_handle(), nullptr);
        ASSERT_EQ(listener.listen(uri), 0);

        std::thread       server_thread([&] {
            qb::io::tcp::ssl::socket server_socket;
            ASSERT_EQ(listener.accept(server_socket), 0);
            drive_server_handshake(server_socket);
            char marker = 0;
            if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
                return;
            }
            EXPECT_EQ(marker, 'u');
            if (!record_thread_failure(write_exactly(server_socket, "s", 1))) {
                return;
            }
        });
        thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

        qb::io::tcp::ssl::socket client;
        client.set_insecure();
        ASSERT_EQ(client.connect(uri, 1s), 0);
        ASSERT_TRUE(client.handshake_complete());
        ASSERT_TRUE(write_exactly(client, "u", 1));
        char reply = 0;
        ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
        EXPECT_EQ(reply, 's');
        client.disconnect();
        listener.disconnect();
        std::remove(path.c_str());
    }
#endif
}

// ===========================================================================
// Non-blocking connect: prepares SSL state, AND completes to a handshake
// ===========================================================================

TEST(SSLSocketLoopback, NonBlockingConnectVariantsPrepareSslState) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    auto expect_progress = [](int ret, int err) {
        EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
            << "unexpected n_connect result=" << ret << " errno=" << err;
    };

    qb::io::tcp::ssl::socket endpoint_client;
    endpoint_client.set_insecure();
    ASSERT_TRUE(endpoint_client.set_sni_hostname("localhost"));
    ASSERT_TRUE(endpoint_client.set_alpn_protocols({"h2"}));
    const int endpoint_ret = endpoint_client.n_connect(qb::io::endpoint("127.0.0.1", port), "localhost");
    expect_progress(endpoint_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(endpoint_client.ssl_handle(), nullptr);
    EXPECT_FALSE(endpoint_client.handshake_complete());
    endpoint_client.close();

    qb::io::tcp::ssl::socket uri_client;
    uri_client.set_insecure();
    const int uri_ret = uri_client.n_connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port)));
    expect_progress(uri_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(uri_client.ssl_handle(), nullptr);
    uri_client.close();

    qb::io::tcp::ssl::socket v4_client;
    v4_client.set_insecure();
    const int v4_ret = v4_client.n_connect_v4("127.0.0.1", port);
    expect_progress(v4_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(v4_client.ssl_handle(), nullptr);
    v4_client.close();

    if (ipv6_loopback_available()) {
        qb::io::tcp::listener ipv6_listener;
        ASSERT_EQ(ipv6_listener.listen_v6(0, "::1"), qb::io::SocketStatus::Done);
        const auto ipv6_port = ipv6_listener.local_endpoint().port();
        ASSERT_NE(ipv6_port, 0);

        qb::io::tcp::ssl::socket v6_client;
        v6_client.set_insecure();
        const int v6_ret = v6_client.n_connect_v6("::1", ipv6_port);
        expect_progress(v6_ret, qb::io::socket::get_last_errno());
        EXPECT_NE(v6_client.ssl_handle(), nullptr);
        v6_client.close();
    }

#ifndef _WIN32
    const auto path = std::string("/tmp/qb-ssl-socket-nconnect-") + std::to_string(::getpid()) + ".sock";
    const auto uri  = qb::io::uri("unix://" + path);
    std::remove(path.c_str());

    qb::io::tcp::listener unix_listener;
    ASSERT_EQ(unix_listener.listen_un(path), qb::io::SocketStatus::Done);

    qb::io::tcp::ssl::socket unix_uri_client;
    unix_uri_client.set_insecure();
    const int unix_uri_ret = unix_uri_client.n_connect(uri);
    expect_progress(unix_uri_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(unix_uri_client.ssl_handle(), nullptr);
    unix_uri_client.close();

    qb::io::tcp::ssl::socket unix_direct_client;
    unix_direct_client.set_insecure();
    const int unix_direct_ret = unix_direct_client.n_connect_un(path);
    expect_progress(unix_direct_ret, qb::io::socket::get_last_errno());
    EXPECT_NE(unix_direct_client.ssl_handle(), nullptr);
    unix_direct_client.close();

    unix_listener.disconnect();
    std::remove(path.c_str());
#endif
}

// Positively verify the non-blocking SUCCESS leg: drive one n_connect variant all
// the way to a completed handshake against a real TLS server (the matrix above
// only checks the in-progress leg / SSL-state preparation).
TEST(SSLSocketLoopback, DriveNonBlockingConnectToCompletion) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect_v4("127.0.0.1", port);
    const int err = qb::io::socket::get_last_errno();
    ASSERT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect_v4 result=" << ret << " errno=" << err;
    EXPECT_NE(client.ssl_handle(), nullptr);

    // Pump the client-side handshake to completion under a deadline.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int        status   = client.handshake_status();
    while (status == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        status = client.handshake_status();
    }
    EXPECT_EQ(status, 1) << "non-blocking TLS handshake never completed; last status=" << status;
    EXPECT_TRUE(client.handshake_complete());
    EXPECT_FALSE(client.get_negotiated_tls_version().empty());
    client.disconnect();
}

// ===========================================================================
// Helpers for the SAN / accessor coverage additions below.
// ===========================================================================

namespace {

// Generate a self-signed EC certificate carrying DNS + IPv4 + IPv6 Subject
// Alternative Names entirely in memory, write the cert+key to temp PEM files,
// and return a server SSL_CTX built from them. The SANs let the loopback peer
// drive the SAN-extraction loops in get_certificate()/get_peer_certificate_chain().
// Returns nullptr (and clears *out_cert_path/out_key_path) on any OpenSSL failure
// so the caller can ASSERT and skip rather than fake-cover.
SSL_CTX *
make_san_server_context(std::string &out_cert_path, std::string &out_key_path) {
    out_cert_path.clear();
    out_key_path.clear();

    EVP_PKEY *pkey = EVP_EC_gen("P-256");
    if (!pkey)
        return nullptr;

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        return nullptr;
    }

    bool ok = true;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    ok = ok && X509_set_pubkey(x509, pkey) == 1;

    X509_NAME *name = X509_get_subject_name(x509);
    ok = ok && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("qb-san-test"), -1, -1, 0) == 1;
    ok = ok && X509_set_issuer_name(x509, name) == 1;

    // DNS + IPv4 + IPv6 SANs, each exercising a different branch of the extractor.
    if (ok) {
        GENERAL_NAMES *gens = sk_GENERAL_NAME_new_null();
        if (gens) {
            auto add_dns = [&](const char *dns) {
                GENERAL_NAME   *gen = GENERAL_NAME_new();
                ASN1_IA5STRING *ia5 = ASN1_IA5STRING_new();
                if (gen && ia5 && ASN1_STRING_set(ia5, dns, static_cast<int>(std::char_traits<char>::length(dns))) == 1) {
                    GENERAL_NAME_set0_value(gen, GEN_DNS, ia5);
                    sk_GENERAL_NAME_push(gens, gen);
                } else {
                    if (ia5)
                        ASN1_IA5STRING_free(ia5);
                    if (gen)
                        GENERAL_NAME_free(gen);
                }
            };
            auto add_ip = [&](const unsigned char *raw, int len) {
                GENERAL_NAME      *gen = GENERAL_NAME_new();
                ASN1_OCTET_STRING *oct = ASN1_OCTET_STRING_new();
                if (gen && oct && ASN1_OCTET_STRING_set(oct, raw, len) == 1) {
                    GENERAL_NAME_set0_value(gen, GEN_IPADD, oct);
                    sk_GENERAL_NAME_push(gens, gen);
                } else {
                    if (oct)
                        ASN1_OCTET_STRING_free(oct);
                    if (gen)
                        GENERAL_NAME_free(gen);
                }
            };

            add_dns("san.example.test");
            const unsigned char ipv4[4] = {127, 0, 0, 1};
            add_ip(ipv4, 4);
            const unsigned char ipv6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}; // ::1
            add_ip(ipv6, 16);

            if (X509_add1_ext_i2d(x509, NID_subject_alt_name, gens, 0, 0) != 1)
                ok = false;
            GENERAL_NAMES_free(gens);
        } else {
            ok = false;
        }
    }

    ok = ok && X509_sign(x509, pkey, EVP_sha256()) != 0;

    if (ok) {
        const std::string base = std::string("/tmp/qb-ssl-san-") + std::to_string(::getpid());
        out_cert_path          = base + "-cert.pem";
        out_key_path           = base + "-key.pem";

        FILE *cf = std::fopen(out_cert_path.c_str(), "wb");
        FILE *kf = std::fopen(out_key_path.c_str(), "wb");
        if (cf && kf && PEM_write_X509(cf, x509) == 1 && PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
            // ok
        } else {
            ok = false;
        }
        if (cf)
            std::fclose(cf);
        if (kf)
            std::fclose(kf);
    }

    SSL_CTX *ctx = nullptr;
    if (ok) {
        ctx = qb::io::ssl::create_server_context(TLS_server_method(), out_cert_path, out_key_path);
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);

    if (!ctx) {
        if (!out_cert_path.empty())
            std::remove(out_cert_path.c_str());
        if (!out_key_path.empty())
            std::remove(out_key_path.c_str());
        out_cert_path.clear();
        out_key_path.clear();
    }
    return ctx;
}

} // namespace

// ===========================================================================
// SAN extraction: a peer cert carrying DNS + IPv4 + IPv6 SANs drives the
// subject-alternative-name loops in get_certificate()/get_peer_certificate_chain().
// ===========================================================================

TEST(SSLSocketLoopback, PeerCertificateSubjectAlternativeNamesAreExtracted) {
    std::string cert_path;
    std::string key_path;
    SSL_CTX    *san_ctx = make_san_server_context(cert_path, key_path);
    ASSERT_NE(san_ctx, nullptr) << "failed to synthesize a SAN-bearing self-signed certificate";

    struct PemCleanup {
        std::string c, k;
        ~PemCleanup() {
            if (!c.empty())
                std::remove(c.c_str());
            if (!k.empty())
                std::remove(k.c_str());
        }
    } pem_cleanup{cert_path, key_path};

    qb::io::tcp::ssl::listener listener;
    listener.init(san_ctx);
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        // Keep the connection alive until the client has inspected the cert.
        char marker = 0;
        record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)));
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());

    const auto details = client.get_peer_certificate_details();
    ASSERT_FALSE(details.subject_alternative_names.empty()) << "SAN extraction produced no entries from a SAN-bearing certificate";

    bool saw_dns  = false;
    bool saw_ipv4 = false;
    bool saw_ipv6 = false;
    for (const auto &san : details.subject_alternative_names) {
        if (san == "DNS:san.example.test")
            saw_dns = true;
        else if (san == "IP:127.0.0.1")
            saw_ipv4 = true;
        else if (san.rfind("IP:", 0) == 0 && san.find(':', 3) != std::string::npos)
            saw_ipv6 = true; // colon-grouped IPv6 form
    }
    EXPECT_TRUE(saw_dns) << "DNS SAN not extracted";
    EXPECT_TRUE(saw_ipv4) << "IPv4 SAN not extracted";
    EXPECT_TRUE(saw_ipv6) << "IPv6 SAN not extracted";

    // The leaf cert also appears (with its SANs) in the verified chain accessor.
    const auto chain = client.get_peer_certificate_chain();
    ASSERT_FALSE(chain.empty());
    bool chain_has_sans = false;
    for (const auto &c : chain) {
        if (!c.subject_alternative_names.empty()) {
            chain_has_sans = true;
            break;
        }
    }
    EXPECT_TRUE(chain_has_sans) << "chain accessor did not extract SANs";

    // Unblock the server's read so the thread exits cleanly.
    EXPECT_TRUE(write_exactly(client, "x", 1));
    client.disconnect();
}

// ===========================================================================
// Post-handshake accessors over a live connection: channel binding, error
// string, OCSP request flag, session-resumption disable, verify depth/callback.
// ===========================================================================

TEST(SSLSocketLoopback, PostHandshakeAccessorsAndChannelBinding) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        char marker = 0;
        record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)));
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // Bring our own client SSL so a handle exists BEFORE the handshake: that lets
    // request_ocsp_stapling(true) actually set the status-request extension
    // (SSL_set_tlsext_status_type), which requires a live SSL object.
    qb::io::tcp::ssl::socket client;
    auto                    *client_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    client.init(SSL_new(client_ctx)); // socket takes ownership of the client SSL+CTX
    ASSERT_NE(client.ssl_handle(), nullptr);
    client.set_insecure();
    ASSERT_TRUE(client.request_ocsp_stapling(true));
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());

    // RFC 5929 tls-server-end-point: a non-empty digest of the peer (server) cert.
    const auto binding = client.tls_server_end_point();
    EXPECT_FALSE(binding.empty()) << "tls-server-end-point channel binding is empty";
    // The shipped self-signed cert is SHA-256-signed, so the binding hash is 32 bytes.
    EXPECT_EQ(binding.size(), 32u);

    // No ALPN was offered, so a CONNECTED socket reports an empty selected
    // protocol (the data==nullptr / len==0 branch, distinct from the no-handle guard).
    EXPECT_TRUE(client.get_alpn_selected_protocol().empty());

    // get_last_ssl_error_string: with a live handle and (typically) an empty error
    // queue, returns the no-error sentinel rather than the no-handle one.
    const auto err_str = client.get_last_ssl_error_string();
    EXPECT_NE(err_str, "No SSL handle");
    EXPECT_FALSE(err_str.empty());

    // disable_session_resumption succeeds on a live handle and clears the session.
    EXPECT_TRUE(client.disable_session_resumption());

    // verify depth / callback setters operate on the live SSL object.
    EXPECT_TRUE(client.set_verify_depth(4));
    EXPECT_TRUE(client.set_verify_callback([](int ok, X509_STORE_CTX *) -> int { return ok; }, SSL_VERIFY_PEER));

    // request_ocsp_stapling(false) is a no-op that still reports success.
    EXPECT_TRUE(client.request_ocsp_stapling(false));

    EXPECT_TRUE(write_exactly(client, "x", 1));
    client.disconnect();
}

// ===========================================================================
// No-handle guards: every accessor returns its documented empty/false form
// when called on a default-constructed (no SSL handle) socket.
// ===========================================================================

TEST(SSLSocketLoopback, AccessorsOnSocketWithoutHandleReturnEmpty) {
    qb::io::tcp::ssl::socket bare;

    EXPECT_EQ(bare.ssl_handle(), nullptr);
    EXPECT_FALSE(bare.handshake_complete());
    EXPECT_EQ(bare.handshake_status(), -1);

    EXPECT_TRUE(bare.tls_server_end_point().empty());
    EXPECT_TRUE(bare.get_peer_certificate_details().subject.empty());
    EXPECT_TRUE(bare.get_peer_certificate_chain().empty());
    EXPECT_TRUE(bare.get_negotiated_cipher_suite().empty());
    EXPECT_TRUE(bare.get_negotiated_tls_version().empty());
    EXPECT_TRUE(bare.get_alpn_selected_protocol().empty());
    EXPECT_EQ(bare.get_last_ssl_error_string(), "No SSL handle");
    EXPECT_FALSE(bare.get_session().is_valid());

    // Mutators that require a handle all fail closed.
    EXPECT_FALSE(bare.disable_session_resumption());
    EXPECT_FALSE(bare.request_ocsp_stapling(true));
    EXPECT_FALSE(bare.set_verify_depth(2));
    EXPECT_FALSE(bare.set_verify_callback(nullptr, SSL_VERIFY_NONE));
    EXPECT_FALSE(bare.request_client_post_handshake_auth());

    qb::io::ssl::Session empty_session{};
    EXPECT_FALSE(bare.set_session(empty_session));

    // read/write without a handle return -1.
    char buf[4] = {};
    EXPECT_EQ(bare.read(buf, sizeof(buf)), -1);
    EXPECT_EQ(bare.write("ab", 2), -1);

    // connected() without a handle is a -1 fast-path (no fd attach).
    EXPECT_EQ(bare.connected(), -1);
}

// ===========================================================================
// connected(): the async-style finalizer attaches the fd to the SSL object and
// drives the handshake on an n_connect'd socket (distinct from handshake_status).
// ===========================================================================

TEST(SSLSocketLoopback, ConnectedFinalizerDrivesNonBlockingHandshake) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect_v4("127.0.0.1", port);
    const int err = qb::io::socket::get_last_errno();
    ASSERT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect_v4 result=" << ret << " errno=" << err;
    ASSERT_NE(client.ssl_handle(), nullptr);

    // Drive the handshake exclusively through connected() (it re-attaches the fd
    // and calls handCheck), under a deadline.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int        rc       = client.connected();
    while (rc == 0 && !client.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        rc = client.connected();
    }
    EXPECT_EQ(rc, 0) << "connected() reported a fatal handshake error";
    EXPECT_TRUE(client.handshake_complete());
    EXPECT_FALSE(client.get_negotiated_cipher_suite().empty());
    client.disconnect();
}

#ifndef _WIN32
// ===========================================================================
// n_connect(uri) AF_UNIX leg: the URI dispatch routes a unix:// URI through
// n_connect_un(), preparing SSL state on the unix-domain transport.
// ===========================================================================

TEST(SSLSocketLoopback, NonBlockingUriUnixLegPreparesSslState) {
    const auto path = std::string("/tmp/qb-ssl-nconnect-uri-unix-") + std::to_string(::getpid()) + ".sock";
    const auto uri  = qb::io::uri("unix://" + path);
    std::remove(path.c_str());

    qb::io::tcp::listener unix_listener;
    ASSERT_EQ(unix_listener.listen_un(path), qb::io::SocketStatus::Done);

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    const int ret = client.n_connect(uri); // AF_UNIX dispatch -> n_connect_un
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect(unix uri) result=" << ret << " errno=" << err;
    EXPECT_NE(client.ssl_handle(), nullptr);
    client.close();

    unix_listener.disconnect();
    std::remove(path.c_str());
}

// ===========================================================================
// connect(uri, timeout) AF_UNIX leg: the timed blocking URI dispatch routes a
// unix:// URI through the endpoint().as_un() + timeout connect path and
// completes a real handshake over the unix-domain transport.
// ===========================================================================

TEST(SSLSocketLoopback, BlockingTimedUriUnixLegCompletesHandshake) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    const auto path = std::string("/tmp/qb-ssl-connect-uri-unix-timed-") + std::to_string(::getpid()) + ".sock";
    const auto uri  = qb::io::uri("unix://" + path);
    std::remove(path.c_str());

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen(uri), 0);

    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        char marker = 0;
        if (!record_thread_failure(read_exactly(server_socket, &marker, sizeof(marker)))) {
            return;
        }
        EXPECT_EQ(marker, 'q');
        record_thread_failure(write_exactly(server_socket, "r", 1));
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    ASSERT_EQ(client.connect(uri, 1s), 0); // unix:// + timeout dispatch leg
    ASSERT_TRUE(client.handshake_complete());
    ASSERT_TRUE(write_exactly(client, "q", 1));
    char reply = 0;
    ASSERT_TRUE(read_exactly(client, &reply, sizeof(reply)));
    EXPECT_EQ(reply, 'r');
    client.disconnect();
    listener.disconnect();
    std::remove(path.c_str());
}
#endif

// ===========================================================================
// init_client(): STARTTLS-style upgrade on an already-connected plaintext fd.
// Prepares client SSL state without a TCP connect, then drives the handshake to
// completion against a TLS server that accepts immediately.
// ===========================================================================

TEST(SSLSocketLoopback, InitClientUpgradesAlreadyConnectedSocket) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    // Establish the plaintext TCP connection first, then move it into an ssl::socket.
    qb::io::tcp::socket plain;
    ASSERT_EQ(plain.connect_v4("127.0.0.1", port), 0);

    qb::io::tcp::ssl::socket upgraded(nullptr, plain);
    upgraded.set_insecure();
    ASSERT_EQ(upgraded.init_client("localhost"), 0); // no TCP connect; SSL state only
    ASSERT_NE(upgraded.ssl_handle(), nullptr);
    EXPECT_FALSE(upgraded.handshake_complete());

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int        status   = upgraded.handshake_status();
    while (status == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        status = upgraded.handshake_status();
    }
    EXPECT_EQ(status, 1) << "init_client() handshake never completed; last status=" << status;
    EXPECT_TRUE(upgraded.handshake_complete());
    EXPECT_FALSE(upgraded.get_negotiated_tls_version().empty());
    upgraded.disconnect();
}

// ===========================================================================
// Post-handshake authentication success leg.
//
// FRAMEWORK CONTRACT (verified against source/io/src/tcp/ssl/socket.cpp:1200): the
// misleadingly-named socket::request_client_post_handshake_auth() wraps OpenSSL's
// SSL_verify_client_post_handshake(), which is a SERVER-ONLY operation — it makes the
// *server* send a post-handshake CertificateRequest to the client. It must therefore be
// called on the SERVER socket, NOT the client. Empirically (see scratch experiment):
//   - on the CLIENT socket it ALWAYS returns false (SSL_verify_client_post_handshake errors
//     on a client SSL object), and
//   - on the SERVER socket it returns true iff ALL of: the server context enabled PHA
//     (enable_post_handshake_auth_server), the client advertised PHA
//     (SSL_CTX_set_post_handshake_auth) before the handshake, TLS 1.3 was negotiated, AND
//     the server context requests the client certificate (SSL_VERIFY_PEER) — without
//     SSL_VERIFY_PEER the call returns false even on the server.
// The flagship negative case (server never enabled PHA -> false) lives elsewhere; this proves
// the SUCCESS return on the only socket/role that can produce it.
// ===========================================================================

TEST(SSLSocketLoopback, PostHandshakeAuthRequestSucceedsWhenServerEnablesIt) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    SSL_CTX *server_ctx = make_server_context();
    ASSERT_NE(server_ctx, nullptr);
    // Server advertises post-handshake auth capability...
    ASSERT_TRUE(qb::io::ssl::enable_post_handshake_auth_server(server_ctx));
    // ...and requests the client certificate. SSL_verify_client_post_handshake() returns 0
    // unless the server is configured to verify the peer; SSL_VERIFY_PEER (without
    // FAIL_IF_NO_PEER_CERT) still lets the initial handshake complete with no client cert.
    SSL_CTX_set_verify(server_ctx, SSL_VERIFY_PEER, nullptr);

    qb::io::tcp::ssl::listener listener;
    listener.init(server_ctx);
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    std::atomic<int>  server_is_tls13{-1};
    std::atomic<int>  server_pha_result{-1};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        server_is_tls13 = (server_socket.get_negotiated_tls_version() == "TLSv1.3") ? 1 : 0;
        // PHA is driven from the SERVER side: the server requests the client to authenticate.
        // The call is server-local — it stages a post-handshake CertificateRequest and reports
        // whether OpenSSL accepted the request — so we record the result right after the
        // handshake without exchanging any further application data (pushing app bytes through a
        // connection mid-PHA is fragile and not what this test verifies). The client side is held
        // open until we publish the result so the connection is still alive during the call.
        server_pha_result = server_socket.request_client_post_handshake_auth() ? 1 : 0;
        server_done       = true;
        server_socket.disconnect();
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // Client must advertise PHA support (SSL_CTX_set_post_handshake_auth) BEFORE
    // the handshake, hence a caller-owned client context + init().
    qb::io::tcp::ssl::socket client;
    auto                    *client_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
    SSL_CTX_set_post_handshake_auth(client_ctx, 1);
#endif
    client.init(SSL_new(client_ctx));
    ASSERT_NE(client.ssl_handle(), nullptr);
    client.set_insecure();
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());

    // Calling it on the CLIENT socket is the wrong role and must always fail — the operation
    // is server-only. This pins the asymmetry, not just the happy path.
    EXPECT_FALSE(client.request_client_post_handshake_auth())
        << "request_client_post_handshake_auth() is a server-side operation; it must fail on the client";

    // Hold the connection open until the server has recorded its PHA result (bounded wait — no
    // unbounded block; the server records immediately after the handshake).
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!server_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    client.disconnect();
    server_thread.join();

    ASSERT_TRUE(server_done.load()) << "server never recorded a PHA result within the deadline";
    ASSERT_EQ(server_is_tls13.load(), 1) << "PHA requires a TLS 1.3 negotiation";
    EXPECT_EQ(server_pha_result.load(), 1) << "server-side PHA request failed despite PHA enabled on the server, advertised by the "
                                              "client, TLS 1.3 negotiated, and SSL_VERIFY_PEER set";
}

// ===========================================================================
// Literal-IP SNI target: connect_v4 with an IP host string drives the
// inet_pton / IP-SAN verification branch of apply_client_peer_verification.
// Because the shipped cert has no IP SAN, the verifying handshake must FAIL,
// while the IP-targeted insecure handshake succeeds (same server).
// ===========================================================================

TEST(SSLSocketLoopback, LiteralIpVerificationBranchRejectsCertWithoutIpSan) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> stop{false};
    std::thread       acceptor([&] {
        for (int i = 0; i < 2 && !stop.load(); ++i) {
            qb::io::tcp::ssl::socket server_socket;
            if (listener.accept(server_socket) != 0) {
                continue;
            }
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!server_socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
                if (server_socket.handshake_status() < 0) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
        }
    });
    thread_join_guard acceptor_join(acceptor, [&] {
        stop = true;
        listener.disconnect();
    });

    // Verifying client targeting a LITERAL IP: hits the inet_pton IP branch and
    // installs IP-SAN matching. The self-signed test cert has no IP SAN, so the
    // chain-valid-but-IP-mismatch handshake MUST be rejected.
    {
        qb::io::tcp::ssl::socket verifying_client;
        ASSERT_TRUE(verifying_client.verify_peer());
        const int ret = verifying_client.connect_v4("127.0.0.1", port);
        EXPECT_NE(ret, 0) << "verifying client accepted a cert lacking the target IP SAN";
        EXPECT_FALSE(verifying_client.handshake_complete());
    }

    // The same literal-IP target succeeds once verification is opted out.
    {
        qb::io::tcp::ssl::socket insecure_client;
        insecure_client.set_insecure();
        const int ret = insecure_client.connect_v4("127.0.0.1", port);
        EXPECT_EQ(ret, 0);
        EXPECT_TRUE(insecure_client.handshake_complete());
        insecure_client.disconnect();
    }
}

// ===========================================================================
// SSL read() must map a clean close_notify (SSL_read() == 0 /
// SSL_ERROR_ZERO_RETURN) to -1, NOT to 0. After a completed handshake the server
// performs a graceful TLS shutdown; the client's next read must report
// termination (-1) so the io layer can dispose the connection rather than spin on
// EV_READ forever. (Drives the SSL_read()==0 -> return -1 branch.)
// ===========================================================================

TEST(SSLSocketLoopback, ReadReportsCleanCloseNotifyAsTermination) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    listener.init(make_server_context());
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        // Send a single byte the client first consumes, then perform a clean TLS
        // shutdown so the client observes close_notify on its following read.
        record_thread_failure(write_exactly(server_socket, "z", 1));
        server_socket.disconnect();
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    qb::io::tcp::ssl::socket client;
    client.set_insecure();
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());

    char first = 0;
    ASSERT_TRUE(read_exactly(client, &first, 1));
    EXPECT_EQ(first, 'z');

    // After the server's graceful close, the client's read must converge to -1
    // (clean close_notify -> termination), never lingering at 0.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int        ret      = 0;
    char       buffer[16];
    while (std::chrono::steady_clock::now() < deadline) {
        ret = client.read(buffer, sizeof(buffer));
        if (ret < 0) {
            break; // termination signalled
        }
        EXPECT_EQ(ret, 0) << "read returned " << ret << " bytes after the peer closed cleanly";
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(ret, -1) << "clean close_notify must be reported as -1, not " << ret;
    client.disconnect();
}

// ===========================================================================
// TLS 1.2 negotiation: capping BOTH peers at TLS 1.2 drives the version branch of
// request_client_post_handshake_auth() (PHA is a TLS 1.3-only feature, so the
// server-side request must return false when 1.2 is negotiated — distinct from
// the TLS 1.3 success leg proven elsewhere). The server context is additionally
// configured via set_tls_protocol_versions() and configure_ecdh_curves_server(),
// exercising the socket-driven CTX configuration on a real handshake.
// ===========================================================================

TEST(SSLSocketLoopback, Tls12NegotiationRejectsServerPostHandshakeAuth) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    SSL_CTX *server_ctx = make_server_context();
    ASSERT_NE(server_ctx, nullptr);
    // Cap the server at TLS 1.2 on both ends of the version window.
    ASSERT_TRUE(qb::io::ssl::set_tls_protocol_versions(server_ctx, TLS1_2_VERSION, TLS1_2_VERSION));
    // Configure the server's ECDH curve list (socket-driven CTX configuration).
    EXPECT_TRUE(qb::io::ssl::configure_ecdh_curves_server(server_ctx, "P-256:X25519"));
    // PHA capability + peer-cert request, mirroring the TLS 1.3 success test — but
    // 1.2 makes the post-handshake request impossible regardless.
    ASSERT_TRUE(qb::io::ssl::enable_post_handshake_auth_server(server_ctx));
    SSL_CTX_set_verify(server_ctx, SSL_VERIFY_PEER, nullptr);

    qb::io::tcp::ssl::listener listener;
    listener.init(server_ctx);
    ASSERT_NE(listener.ssl_handle(), nullptr);
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> server_ready{false};
    std::atomic<bool> server_done{false};
    std::atomic<int>  server_version_is_12{-1};
    std::atomic<int>  server_pha_result{-1};
    std::thread       server_thread([&] {
        qb::io::tcp::ssl::socket server_socket;
        server_ready = true;
        ASSERT_EQ(listener.accept(server_socket), 0);
        drive_server_handshake(server_socket);
        server_version_is_12 = (server_socket.get_negotiated_tls_version() == "TLSv1.2") ? 1 : 0;
        // Server-side PHA over a TLS 1.2 connection must fail at the version guard.
        server_pha_result = server_socket.request_client_post_handshake_auth() ? 1 : 0;
        server_done       = true;
        server_socket.disconnect();
    });
    thread_join_guard server_join(server_thread, [&] { listener.disconnect(); });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // Client also capped at TLS 1.2 so the negotiation lands on 1.2.
    qb::io::tcp::ssl::socket client;
    auto                    *client_ctx = qb::io::ssl::create_client_context(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    ASSERT_TRUE(qb::io::ssl::set_tls_protocol_versions(client_ctx, TLS1_2_VERSION, TLS1_2_VERSION));
    client.init(SSL_new(client_ctx));
    ASSERT_NE(client.ssl_handle(), nullptr);
    client.set_insecure();
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
    ASSERT_TRUE(client.handshake_complete());
    EXPECT_EQ(client.get_negotiated_tls_version(), "TLSv1.2");

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!server_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    client.disconnect();
    server_thread.join();

    ASSERT_TRUE(server_done.load()) << "server never recorded a PHA result within the deadline";
    ASSERT_EQ(server_version_is_12.load(), 1) << "negotiation did not land on TLS 1.2";
    EXPECT_EQ(server_pha_result.load(), 0) << "server-side PHA must fail over a TLS 1.2 connection (PHA is TLS 1.3-only)";
}
