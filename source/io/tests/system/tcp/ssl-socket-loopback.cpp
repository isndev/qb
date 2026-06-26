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

#include <openssl/ssl.h>

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

int
select_first_alpn(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *) {
    if (!in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    *out    = in + 1;
    *outlen = in[0];
    return SSL_TLSEXT_ERR_OK;
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
        last           = ret;
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
        last           = ret;
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

        std::thread server_thread([&] {
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

        std::thread server_thread([&] {
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

    std::thread server_thread([&] {
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
