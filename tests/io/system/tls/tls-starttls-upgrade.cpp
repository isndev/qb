/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-starttls-upgrade.cpp
 * @brief The generic opportunistic-TLS / STARTTLS connector primitive — success and failure paths.
 *
 * Promoted from test-async-io.cpp's `StarttlsOpportunisticUpgrade`. It validates
 * `qb::io::async::tcp::starttls_connect()` independently of any protocol: a mock server performs a
 * cleartext negotiation ("STLS" -> verdict) on the bare fd, then (on agreement) a server-side TLS
 * handshake. The client drives connect → negotiate → upgrade asynchronously and ends up on a
 * completed TLS handshake. Both the callback form and the `co_await` form are exercised, and the
 * RFC 5929 `tls-server-end-point` channel-binding hash is asserted non-empty once secure.
 *
 * Added (per the restructure spec §7): a STARTTLS FAILURE path — when the mock server *declines* the
 * upgrade ('N'), the negotiator must report `fail` and the connector must deliver a non-secure,
 * non-open socket (no spurious TLS). This proves the connector does not silently "succeed" on a
 * refused upgrade.
 *
 * De-flake (per the restructure spec §2): the original used fixed port 64390, an 80ms blind sleep
 * before connecting, and `for (i<600) { async::run(EVRUN_ONCE); sleep_for(5ms); }` budgets. Here the
 * mock server binds `:0` (ephemeral), and every wait is the bounded, loud `qb::io::test::pump_until`.
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
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/transport/stcp.h>
#include <qb/io/uri.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/ssl_fixtures.h"

using namespace std::chrono_literals;

using qb::io::test::pump_until;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;

namespace {

// A minimal STARTTLS negotiator for the generic connector primitive: send the
// 4-byte tag "STLS", read a 1-byte verdict, upgrade on 'S' (anything else fails).
// Mirrors PostgreSQL's SSLRequest negotiator with a trivial wire format.
struct test_starttls_negotiator {
    static constexpr bool enabled = true;
    std::size_t           wrote_{0};
    char                  verdict_{0};
    bool                  got_{false};

    qb::io::async::tcp::starttls_action
    advance(qb::io::tcp::socket &s, int) noexcept {
        using A                      = qb::io::async::tcp::starttls_action;
        static constexpr char REQ[4] = {'S', 'T', 'L', 'S'};
        while (wrote_ < sizeof(REQ)) {
            const int n = s.write(REQ + wrote_, sizeof(REQ) - wrote_);
            if (n > 0) {
                wrote_ += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
                return A::want_write;
            }
            return A::fail;
        }
        if (!got_) {
            const int n = s.read(&verdict_, 1);
            if (n == 1) {
                got_ = true;
            } else if (n < 0 && qb::io::socket::not_recv_error(qb::io::socket::get_last_errno())) {
                return A::want_read;
            } else {
                return A::fail;
            }
        }
        return verdict_ == 'S' ? A::upgrade : A::fail;
    }
};

// A negotiator with nothing to say: the upgrade is agreed before a byte is exchanged, so the TLS
// handshake starts on the turn the TCP connect completes.
struct immediate_upgrade_negotiator {
    static constexpr bool enabled = true;

    qb::io::async::tcp::starttls_action
    advance(qb::io::tcp::socket &, int) noexcept {
        return qb::io::async::tcp::starttls_action::upgrade;
    }
};

// An ssl::socket that counts the handshake steps the connector runs (the connector calls
// `Socket_::handshake_status()`, so this one hides the base's). The count is shared: the connector
// moves the socket it is given.
class counting_ssl_socket : public qb::io::tcp::ssl::socket {
public:
    std::shared_ptr<int> handshake_steps = std::make_shared<int>(0);

    counting_ssl_socket() = default; // not an aggregate: the connector's `Socket_{}` is a default construction

    int
    handshake_status() noexcept {
        ++*handshake_steps;
        return qb::io::tcp::ssl::socket::handshake_status();
    }
};

// A mock server: cleartext "STLS" negotiation on the bare fd, replying `verdict`
// ('S' to agree, 'N' to decline). On agreement it then drives a server-side TLS
// handshake. Binds an ephemeral port; runs until `stop`.
class starttls_mock_server {
public:
    starttls_mock_server(char verdict, std::atomic<bool> &stop)
        : _stop(stop) {
        _listener.init(qb::io::ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
        EXPECT_EQ(_listener.listen_v4(0, "127.0.0.1"), 0);
        // Non-blocking accept: a *blocking* accept() cannot be interrupted by
        // disconnect() on macOS, so shutdown()'s join() would hang forever once the
        // thread parks on a connection that never comes (the deterministic failure of
        // the refused-upgrade case). A non-blocking accept returns immediately when no
        // connection is pending, so the loop re-checks _stop every iteration and joins.
        _listener.set_nonblocking(true);
        _port   = _listener.local_endpoint().port();
        _thread = std::thread([this, verdict] {
            const auto to = 2000ms;
            while (!_stop.load()) {
                qb::io::tcp::ssl::socket s;
                if (_listener.accept(s) != 0) {
                    std::this_thread::sleep_for(2ms); // no pending connection — back off and re-check _stop
                    continue;
                }
                char req[4] = {};
                if (qb::io::socket::recv_n(s.native_handle(), req, 4, to) != 4) {
                    continue;
                }
                const bool agree = (std::memcmp(req, "STLS", 4) == 0) && verdict == 'S';
                const char reply = agree ? 'S' : 'N';
                qb::io::socket::send_n(s.native_handle(), &reply, 1, to);
                if (agree) {
                    const auto deadline = std::chrono::steady_clock::now() + 2s;
                    while (s.do_handshake() == 0 && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(1ms);
                    }
                }
            }
        });
    }

    starttls_mock_server(const starttls_mock_server &)            = delete;
    starttls_mock_server &operator=(const starttls_mock_server &) = delete;

    [[nodiscard]] unsigned short
    port() const {
        return _port;
    }

    void
    shutdown() {
        _stop = true;
        _listener.disconnect();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    ~starttls_mock_server() {
        shutdown();
    }

private:
    qb::io::tcp::ssl::listener _listener;
    std::atomic<bool>         &_stop;
    unsigned short             _port{0};
    std::thread                _thread;
};

} // namespace

// The agreed path: both the callback and co_await forms upgrade to a completed
// TLS handshake, and the channel-binding hash is available once secure.
TEST(TlsStarttlsUpgrade, OpportunisticUpgradeCompletesViaCallbackAndCoroutine) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::async::init();
    std::atomic<bool>    stop{false};
    starttls_mock_server server('S', stop);
    ASSERT_NE(server.port(), 0);
    const qb::io::uri remote{"tcp://127.0.0.1:" + std::to_string(server.port())};

    // ---- callback form ----
    bool        cb_done = false, cb_secure = false;
    std::size_t cb_tlsbind = 0;
    qb::io::async::tcp::starttls_connect<qb::io::tcp::ssl::socket, test_starttls_negotiator>(
        remote,
        [&](qb::io::tcp::ssl::socket &&sock) {
            cb_done    = true;
            cb_secure  = sock.is_open() && sock.handshake_complete();
            cb_tlsbind = sock.tls_server_end_point().size();
        },
        5s, /*verify_peer=*/false);
    EXPECT_TRUE(pump_until([&] { return cb_done; }, 5s)) << "callback STARTTLS never completed";
    EXPECT_TRUE(cb_secure) << "callback STARTTLS did not reach a completed TLS handshake";
    EXPECT_GT(cb_tlsbind, 0u) << "tls_server_end_point() returned no channel-binding hash";

    // ---- coroutine (co_await) form ----
    bool coro_done = false, coro_secure = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto sock   = co_await qb::io::async::tcp::starttls_connect<qb::io::transport::stcp, test_starttls_negotiator>(remote, 5s, false);
        coro_done   = true;
        coro_secure = sock.has_value() && sock->is_open() && sock->handshake_complete();
        co_return;
    }());
    EXPECT_TRUE(coro_done);
    EXPECT_TRUE(coro_secure) << "co_await STARTTLS did not reach a completed TLS handshake";

    server.shutdown();
    qb::io::async::listener::current.clear();
}

// The refused path: the server declines the upgrade ('N'), the negotiator returns
// `fail`, and the connector delivers a non-secure / non-open socket rather than
// silently "succeeding" on a connection that was never upgraded.
TEST(TlsStarttlsUpgrade, RefusedUpgradeDeliversNonSecureSocket) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::async::init();
    std::atomic<bool>    stop{false};
    starttls_mock_server server('N', stop); // declines the STARTTLS request
    ASSERT_NE(server.port(), 0);
    const qb::io::uri remote{"tcp://127.0.0.1:" + std::to_string(server.port())};

    bool cb_done   = false;
    bool cb_secure = true; // must be flipped to false by the refused upgrade
    qb::io::async::tcp::starttls_connect<qb::io::tcp::ssl::socket, test_starttls_negotiator>(
        remote,
        [&](qb::io::tcp::ssl::socket &&sock) {
            cb_done   = true;
            cb_secure = sock.is_open() && sock.handshake_complete();
        },
        5s, /*verify_peer=*/false);

    EXPECT_TRUE(pump_until([&] { return cb_done; }, 5s)) << "refused STARTTLS callback never fired";
    EXPECT_FALSE(cb_secure) << "a declined STARTTLS upgrade must not yield a completed TLS handshake";

    server.shutdown();
    qb::io::async::listener::current.clear();
}

// A pending TLS handshake is watched in the ONE direction OpenSSL waits on (Huly QB-301). After its
// ClientHello the client waits for the server's flight, and a connected socket is always writable: the
// write watch the connector used to keep beside the read one re-ran the handshake on every
// level-triggered loop pass until the server answered. Single-threaded, so the server's silence is an
// order the test writes: the connection waits in the listener's backlog through a hundred loop passes,
// then the test accepts it and drives the server's side. The real ssl::socket reports the direction
// (`handshake_wants_write`), which the connector's mock tests cannot prove. Red before the fix on every
// backend: 3052 steps on epoll (g++), 1649 on wepoll (MSVC), where the fix takes 1.
TEST(TlsStarttlsUpgrade, PendingHandshakeWaitsForTheServerWithoutSpinning) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::async::init();
    qb::io::tcp::ssl::listener listener;
    listener.init(qb::io::ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem")));
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    counting_ssl_socket client;
    client.set_insecure(); // the shipped certificate is self-signed: what is under test is the wait
    const auto steps  = client.handshake_steps;
    bool       done   = false;
    bool       secure = false;
    qb::io::async::tcp::starttls_connect<counting_ssl_socket, immediate_upgrade_negotiator>(
        std::move(client), qb::io::uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](counting_ssl_socket &&sock) {
            done   = true;
            secure = sock.is_open() && sock.handshake_complete();
        },
        5s);

    ASSERT_TRUE(pump_until([&] { return *steps > 0; }, 5s)) << "the TLS handshake never started";
    for (int pass = 0; pass < 100; ++pass)
        qb::io::async::run(EVRUN_NOWAIT);
    EXPECT_EQ(*steps, 1) << "the handshake waiting for the server's flight was re-run on write readiness";
    EXPECT_FALSE(done);

    qb::io::tcp::ssl::socket server;
    ASSERT_EQ(listener.accept(server), 0);
    server.set_nonblocking(true);
    EXPECT_TRUE(pump_until(
        [&] {
            if (!server.handshake_complete())
                server.handshake_status();
            return done;
        },
        5s))
        << "the STARTTLS handshake never completed once the server answered";
    EXPECT_TRUE(secure) << "the upgraded connection did not complete its TLS handshake";

    server.disconnect();
    listener.disconnect();
    qb::io::async::listener::current.clear();
}
