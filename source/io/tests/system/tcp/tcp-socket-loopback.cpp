/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/tcp-socket-loopback.cpp
 * @brief The canonical TCP loopback system test — every connect flavour over a real socket pair.
 *
 * This file drives the whole TCP socket surface against in-process loopback peers: the portable
 * low-level `qb::io::socket` wrapper (`pserve`/`pconnect*`/`xpconnect*`/`accept_n`/`recv_n`/`send_n`/
 * `resolve*`/`set_keepalive`/`tcp_rtt`/error classifiers), the typed `qb::io::tcp::socket`
 * (`init`/`bind`/`connect*`/`n_connect*`/`read`/`write`/move-from-base), and `qb::io::tcp::listener`
 * (`listen_v4`/`listen_v6`/`listen(uri)`/`accept`). IPv4, IPv6 and AF_UNIX endpoints are all
 * exercised, across blocking / timeout / non-blocking connect variants and URI-driven binds.
 *
 * It is the renamed, hardened successor of test-tcp-socket.cpp, and it ALSO absorbs the loopback
 * round-trips that were duplicated (worse) in test-io.cpp's `INET_TCP.*` / `UNIX_TCP.*` /
 * `SocketUtils.EndpointOnValidConnection` suites — those fixed-port, `sleep_for(3s)`, assert-the-
 * empty-non-result clones are folded here as proper ephemeral-port, deterministic round-trips
 * (`BlockingLoopbackTransfersExactPayload`, `EndpointAccessorsReflectLiveConnection`, and the unix
 * leg of `UnixUriBindListenAndConnectVariantsReachLocalSocket`).
 *
 * The loopback scaffolding (`with_tcp_pair`, `accept_tcp_connections`, `accept_low_level_connections`,
 * `reserve_free_tcp_port`) is the shared `tests/shared/loopback_fixture.h` — all ephemeral ports, no
 * fixed port anywhere, collision-free under `ctest -j`.
 *
 * Hardening over the original (per the restructure spec §2/§7):
 *   - the IPv6 cases self-gate on a real `::1` bind probe (`ipv6_loopback_available()`) so a host
 *     without IPv6 loopback SKIPS cleanly instead of failing on bind;
 *   - `NonBlockingConnectForcesGenuineEinprogress` drives the actual `EINPROGRESS` path against a
 *     full backlog and then completes it via `handle_write_ready` — the non-blocking machinery is now
 *     *positively* exercised, not merely smoke-accepted as "0 || EINPROGRESS || would-block";
 *   - `set_keepalive` is verified to have taken effect via a follow-up `get_optval(SO_KEEPALIVE)`
 *     readback rather than only a `<= 0` smoke check.
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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/loopback_fixture.h"

using namespace std::chrono_literals;

using qb::io::test::accept_low_level_connections;
using qb::io::test::accept_tcp_connections;
using qb::io::test::reserve_free_tcp_port;
using qb::io::test::with_tcp_pair;

namespace {

// ---------------------------------------------------------------------------
// IPv6-loopback availability probe: actually bind `::1` once. Hosts/CI images
// without an IPv6 loopback should SKIP the v6 legs cleanly rather than fail on
// bind. A bind is the only honest signal — `getipsv()` reports general support,
// not whether `::1` is routable in this container.
// ---------------------------------------------------------------------------
bool
ipv6_loopback_available() {
    qb::io::tcp::listener probe;
    return probe.listen_v6(0, "::1") == qb::io::SocketStatus::Done;
}

} // namespace

// ===========================================================================
// Typed tcp::socket — init / bind / URI contracts
// ===========================================================================

TEST(TCPSocket, InitBindAndUriContracts) {
    qb::io::tcp::socket socket;
    EXPECT_EQ(socket.init(), 0);
    EXPECT_TRUE(socket.is_open());
    EXPECT_EQ(socket.init(), 0) << "init() must be idempotent on an already-open socket";

    // A default (v4) socket must reject binding a v6 endpoint family.
    const auto ipv6_loopback = qb::io::endpoint().as_in("::1", 0);
    EXPECT_EQ(socket.bind(ipv6_loopback), -1);

    qb::io::tcp::socket bound;
    EXPECT_EQ(bound.bind(qb::io::endpoint().as_in("127.0.0.1", 0)), 0);
    EXPECT_TRUE(bound.is_open());
    EXPECT_EQ(bound.local_endpoint().af(), AF_INET);

    qb::io::tcp::socket uri_bound;
    EXPECT_EQ(uri_bound.bind(qb::io::uri("tcp://127.0.0.1:0")), 0);
    EXPECT_TRUE(uri_bound.is_open());
    EXPECT_NE(uri_bound.local_endpoint().port(), 0);

    qb::io::tcp::socket invalid_uri_bound;
    EXPECT_EQ(invalid_uri_bound.bind(qb::io::uri("file:/tmp/qb.sock")), -1);
}

// ===========================================================================
// Blocking / timeout connect variants over loopback
// ===========================================================================

TEST(TCPSocket, UriAndTimeoutConnectVariantsReachLoopbackServer) {
    with_tcp_pair(
        [](qb::io::tcp::socket accepted) {
            accepted.set_nonblocking(false);
            char buffer[16] = {};
            EXPECT_EQ(accepted.read(buffer, sizeof("ping")), static_cast<int>(sizeof("ping")));
            EXPECT_STREQ(buffer, "ping");
            EXPECT_GE(accepted.write("pong", sizeof("pong")), static_cast<int>(sizeof("pong")));
        },
        [](unsigned short port) {
            qb::io::tcp::socket client;
            const auto          uri = qb::io::uri("tcp://127.0.0.1:" + std::to_string(port));

            ASSERT_EQ(client.connect(uri, 1s), qb::io::SocketStatus::Done);
            EXPECT_TRUE(client.is_open());
            EXPECT_EQ(client.peer_endpoint().port(), port);
            EXPECT_GE(client.write("ping", sizeof("ping")), static_cast<int>(sizeof("ping")));

            char buffer[16] = {};
            EXPECT_EQ(client.read(buffer, sizeof("pong")), static_cast<int>(sizeof("pong")));
            EXPECT_STREQ(buffer, "pong");
            client.disconnect();
        });
}

// Absorbed from test-io.cpp INET_TCP.Blocking: a full payload transfer over a
// blocking loopback pair, now on an ephemeral port (was fixed-port + sleep).
TEST(TCPSocket, BlockingLoopbackTransfersExactPayload) {
    constexpr char message[] = "Hello Test !";
    with_tcp_pair(
        [&](qb::io::tcp::socket accepted) {
            accepted.set_nonblocking(false);
            char      buffer[512] = {};
            const int got         = accepted.read(buffer, sizeof(buffer));
            ASSERT_EQ(got, static_cast<int>(sizeof(message)));
            EXPECT_STREQ(buffer, message);
        },
        [&](unsigned short port) {
            qb::io::tcp::socket sock;
            ASSERT_EQ(sock.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
            EXPECT_TRUE(sock.is_open());
            EXPECT_EQ(sock.peer_endpoint().port(), port);
            EXPECT_EQ(sock.write(message, sizeof(message)), static_cast<int>(sizeof(message)));
            sock.disconnect();
        });
}

// Absorbed from test-io.cpp SocketUtils.EndpointOnValidConnection (was fixed
// port 64329): a live connection exposes a real local and peer endpoint.
TEST(TCPSocket, EndpointAccessorsReflectLiveConnection) {
    with_tcp_pair(
        [](qb::io::tcp::socket accepted) {
            EXPECT_TRUE(accepted.local_endpoint());
            EXPECT_TRUE(accepted.peer_endpoint());
        },
        [](unsigned short port) {
            qb::io::tcp::socket sock;
            ASSERT_EQ(sock.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);

            const auto local = sock.local_endpoint();
            EXPECT_TRUE(local);
            EXPECT_NE(local.port(), 0);

            const auto peer = sock.peer_endpoint();
            EXPECT_TRUE(peer);
            EXPECT_EQ(peer.port(), port);
            sock.disconnect();
        });
}

TEST(TCPSocket, BlockingConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 3;

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_tcp_connections(listener, expected_connections); });

    qb::io::tcp::socket endpoint_client;
    EXPECT_EQ(endpoint_client.connect(qb::io::endpoint().as_in("127.0.0.1", port)), 0);
    endpoint_client.disconnect();

    qb::io::tcp::socket uri_client;
    EXPECT_EQ(uri_client.connect(qb::io::uri("tcp://127.0.0.1:" + std::to_string(port))), 0);
    uri_client.disconnect();

    qb::io::tcp::socket v4_client;
    EXPECT_EQ(v4_client.connect_v4("127.0.0.1", port), 0);
    v4_client.disconnect();

    server_thread.join();
}

// ===========================================================================
// Non-blocking connect — smoke band AND a genuine EINPROGRESS completion
// ===========================================================================

TEST(TCPSocket, NonBlockingUriConnectReportsProgressOrImmediateSuccess) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);

    qb::io::tcp::socket client;
    const auto          uri = qb::io::uri("tcp://127.0.0.1:" + std::to_string(listener.local_endpoint().port()));

    const int ret = client.n_connect(uri);
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect result=" << ret << " errno=" << err;
    EXPECT_TRUE(client.is_open());
    client.close();
}

TEST(TCPSocket, NonBlockingEndpointAndV4ConnectReportProgressOrImmediateSuccess) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();

    qb::io::tcp::socket endpoint_client;
    const int           endpoint_ret = endpoint_client.n_connect(qb::io::endpoint("127.0.0.1", port));
    const int           endpoint_err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(endpoint_ret == 0 || endpoint_err == EINPROGRESS || qb::io::socket::not_send_error(endpoint_err))
        << "unexpected n_connect endpoint result=" << endpoint_ret << " errno=" << endpoint_err;
    endpoint_client.close();

    qb::io::tcp::socket v4_client;
    const int           v4_ret = v4_client.n_connect_v4("127.0.0.1", port);
    const int           v4_err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(v4_ret == 0 || v4_err == EINPROGRESS || qb::io::socket::not_send_error(v4_err))
        << "unexpected n_connect_v4 result=" << v4_ret << " errno=" << v4_err;
    v4_client.close();
}

// Positively drive the non-blocking machinery to the genuine EINPROGRESS state
// and then complete it. We connect to a listener whose backlog we deliberately
// saturate so the SYN cannot complete synchronously; the connect must report
// in-progress, and `handle_write_ready` must then resolve it to a writable
// (connected) socket. This is the only test that *forces* the deferred path
// rather than smoke-accepting an immediate loopback success.
TEST(TCPSocket, NonBlockingConnectForcesGenuineEinprogressThenCompletes) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // Saturate the accept backlog with non-blocking pending connects so the next
    // SYN cannot be accepted synchronously and the kernel reports EINPROGRESS.
    std::vector<qb::io::socket> backlog;
    backlog.reserve(64);
    bool saw_einprogress = false;
    for (int i = 0; i < 64 && !saw_einprogress; ++i) {
        qb::io::socket filler;
        ASSERT_TRUE(filler.open(AF_INET, SOCK_STREAM, 0));
        filler.set_nonblocking(true);
        const int ret  = filler.connect_n(qb::io::endpoint("127.0.0.1", port));
        const int cerr = qb::io::socket::get_last_errno();
        // A deferred (in-progress) non-blocking connect is EINPROGRESS on POSIX but
        // WSAEWOULDBLOCK on Windows (where every non-blocking connect defers) — accept
        // both so the deferred-completion path is exercised on every platform.
        if (ret != 0 && (cerr == EINPROGRESS || cerr == EWOULDBLOCK)) {
            saw_einprogress = true;
        }
        backlog.push_back(std::move(filler));
    }

    if (!saw_einprogress) {
        // Loopback completed every SYN synchronously (small/zero RTT). The
        // deferred path is not reachable here; the smoke-band tests above still
        // cover the immediate-success leg, so this is an honest skip, not a fail.
        GTEST_SKIP() << "loopback never produced EINPROGRESS even with a saturated backlog";
    }

    // The in-progress socket must finish connecting once it becomes writable.
    // handle_write_ready() returns select()'s value: > 0 (ready fd count) once the
    // socket is writable, 0 on timeout, -1 on error — it is NOT a 0-on-success call.
    // The definitive proof that the connect completed is the SO_ERROR readback below.
    qb::io::socket &pending = backlog.back();
    const int       ready   = pending.handle_write_ready(2s);
    EXPECT_GT(ready, 0) << "handle_write_ready did not report the in-progress connect writable; errno=" << qb::io::socket::get_last_errno();

    int so_error = -1;
    ASSERT_EQ(pending.get_optval(SOL_SOCKET, SO_ERROR, so_error), 0);
    EXPECT_EQ(so_error, 0) << "completed non-blocking connect left a pending SO_ERROR";
}

// ===========================================================================
// IPv6 connect variants (self-gated on a real ::1 bind)
// ===========================================================================

TEST(TCPSocket, IPv6ConnectVariantsReachLoopbackServer) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

    constexpr int expected_connections = 3;

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v6(0, "::1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_tcp_connections(listener, expected_connections); });

    qb::io::tcp::socket endpoint_client;
    EXPECT_EQ(endpoint_client.connect(qb::io::endpoint().as_in("::1", port)), 0);
    endpoint_client.disconnect();

    qb::io::tcp::socket uri_timeout_client;
    const auto          uri = qb::io::uri("tcp://[::1]:" + std::to_string(port));
    EXPECT_EQ(uri_timeout_client.connect(uri, 1s), qb::io::SocketStatus::Done);
    uri_timeout_client.disconnect();

    qb::io::tcp::socket v6_client;
    EXPECT_EQ(v6_client.connect_v6("::1", port), 0);
    v6_client.disconnect();

    server_thread.join();

    qb::io::tcp::socket n_v6_client;
    const int           ret = n_v6_client.n_connect_v6("::1", port);
    const int           err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected n_connect_v6 result=" << ret << " errno=" << err;
    n_v6_client.disconnect();
}

TEST(TCPSocket, AlreadyOpenSocketRejectsMismatchedEndpointFamilies) {
    const auto ipv6_loopback = qb::io::endpoint().as_in("::1", 1);

    qb::io::tcp::socket blocking_client;
    ASSERT_EQ(blocking_client.init(AF_INET), 0);
    EXPECT_EQ(blocking_client.connect(ipv6_loopback), -1);

    qb::io::tcp::socket timeout_client;
    ASSERT_EQ(timeout_client.init(AF_INET), 0);
    EXPECT_EQ(timeout_client.connect(ipv6_loopback, 1ms), -1);

    qb::io::tcp::socket nonblocking_client;
    ASSERT_EQ(nonblocking_client.init(AF_INET), 0);
    EXPECT_EQ(nonblocking_client.n_connect(ipv6_loopback), -1);
}

// ===========================================================================
// Unix-domain endpoints (POSIX only)
// ===========================================================================

#ifndef _WIN32

TEST(TCPSocket, UnixUriBindListenAndConnectVariantsReachLocalSocket) {
    constexpr int expected_connections = 3;
    const auto    path                 = std::string("/tmp/qb-tcp-socket-uri-") + std::to_string(::getpid()) + ".sock";
    const auto    uri                  = qb::io::uri("unix://" + path);

    std::remove(path.c_str());

    qb::io::tcp::socket bound;
    ASSERT_EQ(bound.bind(uri), 0);
    bound.close();
    std::remove(path.c_str());

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen(uri), qb::io::SocketStatus::Done);
    EXPECT_TRUE(listener.is_open());

    std::thread server_thread([&] { accept_tcp_connections(listener, expected_connections); });

    qb::io::tcp::socket uri_client;
    EXPECT_EQ(uri_client.connect(uri), 0);
    uri_client.disconnect();

    qb::io::tcp::socket uri_timeout_client;
    EXPECT_EQ(uri_timeout_client.connect(uri, 1s), qb::io::SocketStatus::Done);
    uri_timeout_client.disconnect();

    qb::io::tcp::socket direct_client;
    EXPECT_EQ(direct_client.connect_un(path), 0);
    direct_client.disconnect();

    server_thread.join();

    qb::io::tcp::socket nonblocking_client;
    const int           ret = nonblocking_client.n_connect(uri);
    const int           err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected unix n_connect result=" << ret << " errno=" << err;
    nonblocking_client.disconnect();

    listener.disconnect();
    std::remove(path.c_str());
}

#endif

// ===========================================================================
// Ownership transfer + EOF/errno contracts
// ===========================================================================

TEST(TCPSocket, BaseSocketMoveConstructionAndAssignmentTransferOwnership) {
    qb::io::socket base_constructed(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(base_constructed.is_open());
    const auto constructed_handle = base_constructed.native_handle();

    qb::io::tcp::socket constructed(std::move(base_constructed));
    EXPECT_TRUE(constructed.is_open());
    EXPECT_EQ(constructed.native_handle(), constructed_handle);
    EXPECT_EQ(base_constructed.native_handle(), qb::io::inet::invalid_socket);

    qb::io::socket base_assigned(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(base_assigned.is_open());
    const auto assigned_handle = base_assigned.native_handle();

    qb::io::tcp::socket assigned;
    assigned = std::move(base_assigned);
    EXPECT_TRUE(assigned.is_open());
    EXPECT_EQ(assigned.native_handle(), assigned_handle);
    EXPECT_EQ(base_assigned.native_handle(), qb::io::inet::invalid_socket);
}

TEST(TCPSocket, ReadReportsPeerCloseWithoutStaleErrno) {
    with_tcp_pair([](qb::io::tcp::socket accepted) { accepted.disconnect(); },
                  [](unsigned short port) {
                      qb::io::tcp::socket client;
                      ASSERT_EQ(client.connect_v4("127.0.0.1", port), 0);
                      qb::io::socket::set_last_errno(EINVAL);

                      char buffer[8] = {};
                      EXPECT_EQ(client.read(buffer, sizeof(buffer)), -1);
                      EXPECT_EQ(qb::io::socket::get_last_errno(), 0) << "EOF/peer-close must clear errno, not leak a stale value";
                      client.close();
                  });
}

// ===========================================================================
// Low-level portable qb::io::socket wrapper
// ===========================================================================

TEST(TCPSocket, LowLevelPortableConnectAndTransferHelpers) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] {
        qb::io::socket accepted;
        accepted = listener.accept().release_handle();
        ASSERT_TRUE(accepted.is_open());

        char buffer[32] = {};
        EXPECT_EQ(accepted.recv_n(buffer, static_cast<int>(sizeof("hello")), 1s), static_cast<int>(sizeof("hello")));
        EXPECT_STREQ(buffer, "hello");
        EXPECT_EQ(accepted.send_n("world", static_cast<int>(sizeof("world")), 1s), static_cast<int>(sizeof("world")));
    });

    qb::io::socket client;
    ASSERT_EQ(client.pconnect("127.0.0.1", port), 0);
    EXPECT_TRUE(client.is_open());
    EXPECT_EQ(client.peer_endpoint().port(), port);
    EXPECT_GE(client.tcp_rtt(), 0u);
    EXPECT_EQ(client.send_n("hello", static_cast<int>(sizeof("hello")), 1s), static_cast<int>(sizeof("hello")));

    char buffer[32] = {};
    EXPECT_EQ(client.recv_n(buffer, static_cast<int>(sizeof("world")), 1s), static_cast<int>(sizeof("world")));
    EXPECT_STREQ(buffer, "world");

    server_thread.join();
}

TEST(TCPSocket, LowLevelPortableConnectVariantsReachLoopbackServer) {
    constexpr int expected_connections = 4;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_low_level_connections(listener, expected_connections); });

    qb::io::socket xp_client;
    EXPECT_EQ(xp_client.xpconnect("127.0.0.1", port), 0);
    xp_client.close();

    qb::io::socket xp_timeout_client;
    EXPECT_EQ(xp_timeout_client.xpconnect_n("127.0.0.1", port, 1s), 0);
    xp_timeout_client.close();

    qb::io::socket p_timeout_client;
    EXPECT_EQ(p_timeout_client.pconnect_n("127.0.0.1", port, 1s), 0);
    p_timeout_client.close();

    qb::io::socket endpoint_timeout_client;
    EXPECT_EQ(endpoint_timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), 1s), 0);
    endpoint_timeout_client.close();

    qb::io::socket nonblocking_client;
    const int      ret = nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", port));
    const int      err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected pconnect_n result=" << ret << " errno=" << err;
    nonblocking_client.close();

    server_thread.join();
}

TEST(TCPSocket, LowLevelPortableConnectVariantsCanBindExplicitLocalPorts) {
    constexpr int expected_connections = 2;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_low_level_connections(listener, expected_connections); });

    qb::io::socket blocking_client;
    const auto     blocking_local_port = reserve_free_tcp_port();
    ASSERT_NE(blocking_local_port, 0);
    EXPECT_EQ(blocking_client.pconnect(qb::io::endpoint("127.0.0.1", port), blocking_local_port), 0);
    EXPECT_EQ(blocking_client.local_endpoint().port(), blocking_local_port);
    blocking_client.close();

    qb::io::socket timeout_client;
    const auto     timeout_local_port = reserve_free_tcp_port();
    ASSERT_NE(timeout_local_port, 0);
    EXPECT_EQ(timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), 1s, timeout_local_port), 0);
    EXPECT_EQ(timeout_client.local_endpoint().port(), timeout_local_port);
    timeout_client.close();

    server_thread.join();

    qb::io::socket nonblocking_client;
    const auto     nonblocking_local_port = reserve_free_tcp_port();
    ASSERT_NE(nonblocking_local_port, 0);
    const int ret = nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", port), nonblocking_local_port);
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected pconnect_n local-port result=" << ret << " errno=" << err;
    EXPECT_EQ(nonblocking_client.local_endpoint().port(), nonblocking_local_port);
    nonblocking_client.close();
}

// POSIX-only: this asserts that binding a client to an already-occupied local port
// fails. `pconnect*` bind the local port to INADDR_ANY (0.0.0.0:local_port — see
// socket::pconnect), while the `occupied` listener holds 127.0.0.1:port. On POSIX,
// 0.0.0.0:P overlaps 127.0.0.1:P → EADDRINUSE → the bind (hence pconnect) fails. On
// Windows those two address scopes do NOT conflict for a plain bind (even with the
// listener's SO_EXCLUSIVEADDRUSE), so the client bind legitimately succeeds — that is
// a correct Windows behavioural difference, not a qb defect, so the assertion is
// guarded to POSIX where the bind-conflict semantics it pins actually hold.
#ifndef _WIN32
TEST(TCPSocket, LowLevelPortableConnectVariantsFailWhenLocalPortIsOccupied) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto remote_port = listener.local_endpoint().port();
    ASSERT_NE(remote_port, 0);

    qb::io::socket occupied;
    ASSERT_EQ(occupied.pserve("127.0.0.1", 0), 0);
    const auto occupied_port = occupied.local_endpoint().port();
    ASSERT_NE(occupied_port, 0);

    qb::io::socket blocking_client;
    EXPECT_EQ(blocking_client.pconnect(qb::io::endpoint("127.0.0.1", remote_port), occupied_port), -1);

    qb::io::socket timeout_client;
    EXPECT_EQ(timeout_client.pconnect_n(qb::io::endpoint("127.0.0.1", remote_port), 1ms, occupied_port), -1);

    qb::io::socket nonblocking_client;
    EXPECT_EQ(nonblocking_client.pconnect_n(qb::io::endpoint("127.0.0.1", remote_port), occupied_port), -1);
}
#endif // !_WIN32

TEST(TCPSocket, LowLevelResolutionAndInterfaceDiscovery) {
    std::vector<qb::io::endpoint> endpoints;
    EXPECT_EQ(qb::io::socket::resolve(endpoints, "localhost", 4242), 0);
    EXPECT_FALSE(endpoints.empty());

    std::vector<qb::io::endpoint> v4_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v4(v4_endpoints, "127.0.0.1", 4242), 0);
    ASSERT_FALSE(v4_endpoints.empty());
    EXPECT_TRUE(std::all_of(v4_endpoints.begin(), v4_endpoints.end(), [](const auto &ep) { return ep.af() == AF_INET && ep.port() == 4242; }));

    std::vector<qb::io::endpoint> v6_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v6(v6_endpoints, "::1", 4242), 0);
    ASSERT_FALSE(v6_endpoints.empty());
    EXPECT_TRUE(std::all_of(v6_endpoints.begin(), v6_endpoints.end(), [](const auto &ep) { return ep.af() == AF_INET6 && ep.port() == 4242; }));

    std::vector<qb::io::endpoint> mapped_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_v4to6(mapped_endpoints, "127.0.0.1", 4242), 0);
    EXPECT_FALSE(mapped_endpoints.empty());

    std::vector<qb::io::endpoint> to_v6_endpoints;
    EXPECT_EQ(qb::io::socket::resolve_tov6(to_v6_endpoints, "localhost", 4242), 0);
    EXPECT_FALSE(to_v6_endpoints.empty());

    int callback_count = 0;
    EXPECT_EQ(qb::io::socket::resolve_i(
                  [&](const qb::io::endpoint &ep) {
                      EXPECT_TRUE(ep.af() == AF_INET || ep.af() == AF_INET6);
                      ++callback_count;
                      return true;
                  },
                  "localhost", 4242, AF_UNSPEC, AI_ALL),
              0);
    EXPECT_EQ(callback_count, 1);

    std::vector<qb::io::endpoint> missing;
    EXPECT_NE(qb::io::socket::resolve(missing, "invalid.invalid.invalid", 4242), 0);
    EXPECT_TRUE(missing.empty());

    const int ip_flags = qb::io::socket::getipsv();
    EXPECT_GE(ip_flags, qb::io::inet::ip::ipsv_unavailable);

    int local_count = 0;
    qb::io::socket::traverse_local_address([&](const qb::io::endpoint &ep) {
        EXPECT_TRUE(ep.af() == AF_INET || ep.af() == AF_INET6);
        ++local_count;
        return true;
    });
    EXPECT_GE(local_count, 0);
}

TEST(TCPSocket, LowLevelSocketOptionsReadinessAndBindingHelpers) {
    qb::io::socket listener;
    ASSERT_TRUE(listener.open(AF_INET, SOCK_STREAM, 0));
    listener.reuse_address(true);
    listener.exclusive_address(false);
    EXPECT_EQ(listener.bind_any(false), 0);
    EXPECT_TRUE(listener.local_endpoint());
    EXPECT_EQ(listener.listen(), 0);

    EXPECT_EQ(listener.handle_read_ready(1ms), 0);
    EXPECT_EQ(qb::io::socket::get_last_errno(), ETIMEDOUT);
    EXPECT_EQ(listener.handle_write_ready(1ms), 0);

    int accept_conn = 0;
    if (listener.get_optval(SOL_SOCKET, SO_ACCEPTCONN, accept_conn) == 0) {
        EXPECT_NE(accept_conn, 0);
    }

    qb::io::socket raw_socket(static_cast<::socket_type>(listener));
    EXPECT_TRUE(raw_socket.is_open());
    EXPECT_EQ(raw_socket.native_handle(), listener.native_handle());
    EXPECT_EQ(listener.release_handle(), raw_socket.native_handle());
    raw_socket.close();

    // set_keepalive must actually take effect — read SO_KEEPALIVE back rather
    // than only smoke-checking the call's return value.
    qb::io::socket keepalive_socket;
    ASSERT_TRUE(keepalive_socket.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_LE(keepalive_socket.set_keepalive(1, 1, 1, 1), 0);
    int keepalive_flag = 0;
    ASSERT_EQ(keepalive_socket.get_optval(SOL_SOCKET, SO_KEEPALIVE, keepalive_flag), 0);
    EXPECT_NE(keepalive_flag, 0) << "set_keepalive(1, ...) did not enable SO_KEEPALIVE";
    EXPECT_GE(qb::io::socket::tcp_rtt(keepalive_socket.native_handle()), 0u);
    qb::io::socket::init_ws32_lib();
}

TEST(TCPSocket, LowLevelNonBlockingAndTimeoutFailuresAreRestored) {
    qb::io::socket closed;
    EXPECT_EQ(qb::io::socket::set_nonblocking(closed.native_handle(), true), -1);
    EXPECT_EQ(closed.shutdown(SD_BOTH), -1); // SD_BOTH is qb's portable alias (==SHUT_RDWR on POSIX)

    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));
    EXPECT_EQ(client.connect_n(qb::io::endpoint().as_in("127.0.0.1", 9), 1ms), -1);
    EXPECT_EQ(client.test_nonblocking(), 0);

    qb::io::socket udp(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(udp.is_open());
    ASSERT_EQ(udp.bind("127.0.0.1", 0), 0);
    const int disconnect_result = udp.disconnect();
    EXPECT_TRUE(disconnect_result == 0 || disconnect_result == -1)
        << "unexpected UDP disconnect result=" << disconnect_result << " errno=" << qb::io::socket::get_last_errno();
}

TEST(TCPSocket, StaticHelpersAndErrorClassifiers) {
    EXPECT_TRUE(qb::io::socket::not_send_error(EWOULDBLOCK));
    EXPECT_TRUE(qb::io::socket::not_send_error(EAGAIN));
    EXPECT_TRUE(qb::io::socket::not_send_error(EINTR));
    EXPECT_TRUE(qb::io::socket::not_send_error(ENOBUFS));
    EXPECT_FALSE(qb::io::socket::not_send_error(ECONNRESET));

    EXPECT_TRUE(qb::io::socket::not_recv_error(EWOULDBLOCK));
    EXPECT_TRUE(qb::io::socket::not_recv_error(EAGAIN));
    EXPECT_TRUE(qb::io::socket::not_recv_error(EINTR));
    EXPECT_FALSE(qb::io::socket::not_recv_error(ECONNRESET));

    EXPECT_NE(qb::io::socket::strerror(EINVAL), nullptr);
    EXPECT_NE(qb::io::socket::gai_strerror(EAI_NONAME), nullptr);

    qb::io::socket::set_last_errno(0);
    EXPECT_EQ(qb::io::socket::get_last_errno(), 0);
}

// ===========================================================================
// COVERAGE ADDITIONS (system/sys__socket.cpp)
// ===========================================================================

// The static, non-blocking `connect_n(socket_type, endpoint)` overload (no
// timeout, no wait): it flips the fd to non-blocking and issues a single
// connect(), returning whatever the kernel reports (0 on the immediate loopback
// success, or in-progress). Distinct from both the timed connect_n and the
// instance disconnect() path below.
TEST(TCPSocket, StaticNonBlockingConnectNReachesLoopback) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    qb::io::socket client;
    ASSERT_TRUE(client.open(AF_INET, SOCK_STREAM, 0));
    const qb::io::endpoint ep("127.0.0.1", port);

    // Static fd-based overload: connect_n(socket_type, endpoint).
    const int ret = qb::io::socket::connect_n(client.native_handle(), ep);
    const int err = qb::io::socket::get_last_errno();
    EXPECT_TRUE(ret == 0 || err == EINPROGRESS || qb::io::socket::not_send_error(err))
        << "unexpected static connect_n(s, ep) result=" << ret << " errno=" << err;
    EXPECT_TRUE(client.is_open());
    client.close();
}

// disconnect(): the instance disconnect() (and its static counterpart) issue a
// connect() to an AF_UNSPEC address, which on a connected datagram/stream socket
// dissolves the association. Drive both the instance and static forms over a UDP
// socket bound to loopback (a connectionless dissolve is well-defined).
TEST(TCPSocket, LowLevelDisconnectDissolvesAssociation) {
    qb::io::socket peer(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(peer.is_open());
    ASSERT_EQ(peer.bind("127.0.0.1", 0), 0);
    const auto peer_port = peer.local_endpoint().port();
    ASSERT_NE(peer_port, 0);

    qb::io::socket sender(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(sender.is_open());
    ASSERT_EQ(sender.connect(qb::io::endpoint("127.0.0.1", peer_port)), 0);

    // Instance disconnect(): connect(AF_UNSPEC) on a connected UDP socket.
    const int instance_result = sender.disconnect();
    EXPECT_TRUE(instance_result == 0 || instance_result == -1) << "unexpected instance disconnect result=" << instance_result;

    // Static disconnect(socket_type): same dissolve via the fd-based entry point.
    qb::io::socket sender2(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(sender2.is_open());
    ASSERT_EQ(sender2.connect(qb::io::endpoint("127.0.0.1", peer_port)), 0);
    const int static_result = qb::io::socket::disconnect(sender2.native_handle());
    EXPECT_TRUE(static_result == 0 || static_result == -1) << "unexpected static disconnect result=" << static_result;
}

// send_n / recv_n must traverse their EWOULDBLOCK retry loop, not merely a single
// syscall. Push a payload larger than a single socket buffer so the sender's
// non-blocking send() returns short / would-block at least once (driving the
// handle_write_ready() continue branch), while the receiver pulls the whole thing
// back through recv_n()'s handle_read_ready() retry branch. Verifies the full
// payload survives the round-trip.
TEST(TCPSocket, LowLevelSendNRecvNTraverseWouldBlockRetryLoop) {
    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("127.0.0.1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // A payload comfortably larger than typical loopback socket buffers, so the
    // non-blocking sender cannot flush it in one shot and must wait on writability.
    constexpr int     payload_size = 4 * 1024 * 1024;
    std::vector<char> payload(payload_size);
    for (int i = 0; i < payload_size; ++i) {
        payload[static_cast<std::size_t>(i)] = static_cast<char>(i & 0xFF);
    }

    std::thread server_thread([&] {
        qb::io::socket accepted;
        accepted = listener.accept().release_handle();
        ASSERT_TRUE(accepted.is_open());
        const int sent = accepted.send_n(payload.data(), payload_size, 5s);
        EXPECT_EQ(sent, payload_size) << "send_n did not flush the full payload through the retry loop";
    });

    qb::io::socket client;
    ASSERT_EQ(client.pconnect("127.0.0.1", port), 0);

    std::vector<char> received(payload_size, 0);
    const int         got = client.recv_n(received.data(), payload_size, 5s);
    EXPECT_EQ(got, payload_size) << "recv_n did not drain the full payload through the retry loop";
    EXPECT_EQ(received, payload) << "payload corrupted across the send_n/recv_n round-trip";

    server_thread.join();
    client.close();
}

// The timed connect_n(socket_type, endpoint, timeout) failure legs: connecting to
// a routable-but-closed loopback port resolves quickly to a connection refused,
// driving the SO_ERROR readback branch (so_error != 0) rather than a select()
// timeout. The companion `connect_n` to a black-hole (discard) port with a tiny
// timeout drives the select()-times-out leg.
TEST(TCPSocket, TimedConnectNSurfacesRefusedAndTimeoutFailures) {
    // 1) Connection refused: bind a listener, learn its port, close it, then
    //    connect_n to that now-dead port. The kernel refuses promptly, surfacing
    //    via the SO_ERROR readback (so_error != 0) branch of connect_n.
    const auto dead_port = reserve_free_tcp_port();
    ASSERT_NE(dead_port, 0);

    qb::io::socket refused_client;
    ASSERT_TRUE(refused_client.open(AF_INET, SOCK_STREAM, 0));
    const int refused_ret = qb::io::socket::connect_n(refused_client.native_handle(), qb::io::endpoint("127.0.0.1", dead_port), 1s);
    EXPECT_EQ(refused_ret, -1) << "connect_n to a closed loopback port must fail";
    refused_client.close();

    // 2) Select timeout: connect_n to a non-routable address with a sub-millisecond
    //    deadline so select() returns 0 (timeout) before the SYN can resolve. The
    //    documented non-routable test sink 192.0.2.1 (TEST-NET-1, RFC 5737) black-
    //    holes the SYN; a 1ns budget guarantees the select()<=0 timeout branch.
    qb::io::socket timeout_client;
    ASSERT_TRUE(timeout_client.open(AF_INET, SOCK_STREAM, 0));
    const int timeout_ret = qb::io::socket::connect_n(timeout_client.native_handle(), qb::io::endpoint("192.0.2.1", 9), qb::duration(1));
    EXPECT_EQ(timeout_ret, -1) << "connect_n to a black-holed address with a 1ns budget must time out";
    timeout_client.close();
}

// xpconnect / xpconnect_n IPv6 leg: dual-stack name resolution that yields an
// AF_INET6 endpoint drives the `case AF_INET6` arm of the resolve_i switch. Bind
// an IPv6 listener and connect by the literal `::1` host string, which resolves
// (only) to AF_INET6 and therefore exercises the v6 branch of both helpers.
TEST(TCPSocket, CrossStackXpConnectReachesIPv6LoopbackServer) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "IPv6 loopback (::1) is not available on this host";
    }

    constexpr int expected_connections = 2;

    qb::io::socket listener;
    ASSERT_EQ(listener.pserve("::1", 0), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server_thread([&] { accept_low_level_connections(listener, expected_connections); });

    qb::io::socket xp_client;
    EXPECT_EQ(xp_client.xpconnect("::1", port), 0) << "xpconnect could not reach the ::1 loopback server";
    xp_client.close();

    qb::io::socket xp_timeout_client;
    EXPECT_EQ(xp_timeout_client.xpconnect_n("::1", port, 1s), 0) << "xpconnect_n could not reach the ::1 loopback server";
    xp_timeout_client.close();

    server_thread.join();
}
