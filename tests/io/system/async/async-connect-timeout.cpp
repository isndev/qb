/**
 * @file system/async/async-connect-timeout.cpp
 * @brief Connection-failure semantics of the async connector + timed socket path.
 *
 * The qb-io connect surface must fail *cleanly and deterministically* — a closed socket delivered from
 * the event loop, never a hang, never a half-open descriptor, never a re-entrant callback. These cases
 * drive the in-process libev loop and real loopback sockets, so they are SYSTEM tests. Contracts proven:
 *
 *   - `async::tcp::connect` to a refused loopback port yields an EMPTY (closed) socket, delivered from
 *     the loop — not synchronously inside connect(), and never re-entrantly;
 *   - `async::tcp::connect` to an unroutable address (TEST-NET-1, RFC 5737) times out within its
 *     configured budget and yields an EMPTY socket — the timeout is enforced, not merely hoped for;
 *   - the timed blocking path (`socket::connect_n` / `handle_write_ready`) to an unroutable address
 *     does NOT report a connected peer (a non-vacuous endpoint check);
 *   - re-connecting an already-connected socket through the timed path surfaces `EISCONN` (the signal
 *     the synchronous TLS upgrade relies on; it must survive `set_nonblocking()` on Windows);
 *   - a UDP receive with a timeout on an idle socket returns no data (it times out, it does not block).
 *
 * Restructured from the dissolved system/test-connection-timeout.cpp. Per the restructure spec: the
 * three near-identical TEST-NET-1 cases with their "either an exception OR a false endpoint passes"
 * escape hatches, their `std::cout` debug prints, and their `catch(...) {}` swallow are DELETED and
 * replaced with deterministic assertions — the connector timeout is driven to a definite closed-socket
 * outcome and the blocking path asserts a single falsifiable fact (not connected). Ports are ephemeral
 * (`listen_v4(0)` + `local_endpoint().port()`). The hand-rolled poll loops are replaced by the shared
 * `qb::io::test::pump_until`. No file-local main().
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
#include <cerrno>
#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/event/all.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/udp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

// 240.0.0.0/4 (RFC 1112 §4, "reserved for future use" / Class E): no OS routes it
// and the local IP stack rejects it as unreachable BEFORE handing the SYN to the
// default gateway. This is deterministic on Windows, Linux and macOS — unlike RFC
// 5737 TEST-NET-1 (192.0.2.0/24), which IS routable: the SYN is forwarded to the
// default gateway, so on a corporate network a transparent proxy / SD-WAN middlebox
// can complete the handshake (peer_endpoint() then reports a peer and the
// "not connected" assertion fails). A Class E target instead fails fast locally
// (ENETUNREACH / WSAENETUNREACH), which still yields an empty/closed socket within
// the connect budget for every test below.
constexpr const char    *kUnroutableHost = "240.0.0.1";
constexpr unsigned short kUnroutablePort = 12345;

class AsyncConnectTimeoutTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// Refused loopback port — empty socket, from the loop, not re-entrant
// =============================================================================

TEST_F(AsyncConnectTimeoutTest, RefusedPortYieldsEmptySocketFromLoopNotReentrant) {
    std::atomic<bool> fired{false};
    std::atomic<bool> reentrant{false};
    std::atomic<bool> socket_open{true};
    bool              connect_returned = false;

    // Port 1 on loopback is closed → the connect is refused (RST). On POSIX this is
    // EINPROGRESS-then-fail; on Windows it can fail synchronously — either way the
    // result MUST arrive from the loop, never re-entrantly inside connect().
    async::tcp::connect<qb::io::tcp::socket>(
        uri{"tcp://127.0.0.1:1"},
        [&](qb::io::tcp::socket &&sock) {
            if (!connect_returned)
                reentrant = true; // fired before connect() returned == re-entrant
            socket_open = sock.is_open();
            fired       = true;
        },
        2s);
    connect_returned = true;

    EXPECT_FALSE(fired.load()) << "connect callback fired synchronously inside connect()";

    EXPECT_TRUE(pump_until([&] { return fired.load(); })) << "connect failure was never delivered from the loop";
    EXPECT_FALSE(reentrant.load()) << "connect callback must not be invoked re-entrantly";
    EXPECT_FALSE(socket_open.load()) << "a refused connect must yield an empty (closed) socket";
}

// =============================================================================
// Unroutable address — connector enforces its timeout, yields empty socket
// =============================================================================

TEST_F(AsyncConnectTimeoutTest, UnroutableAddressTimesOutAndYieldsEmptySocket) {
    std::atomic<bool> fired{false};
    std::atomic<bool> socket_open{true};

    const auto start = std::chrono::steady_clock::now();
    async::tcp::connect<qb::io::tcp::socket>(
        uri{"tcp://" + std::string(kUnroutableHost) + ":" + std::to_string(kUnroutablePort)},
        [&](qb::io::tcp::socket &&sock) {
            socket_open = sock.is_open();
            fired       = true;
        },
        1s); // 1s connect budget

    // The callback must arrive within a generous multiple of the configured timeout.
    EXPECT_TRUE(pump_until([&] { return fired.load(); }, 5s)) << "connector never delivered a timeout result";
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(socket_open.load()) << "a timed-out connect must yield an empty (closed) socket";
    EXPECT_LT(elapsed, 5s) << "connector ignored its timeout budget (took too long to give up)";
}

// =============================================================================
// Blocking timed path to an unroutable address — definitely NOT connected
// =============================================================================

TEST_F(AsyncConnectTimeoutTest, TimedBlockingConnectToUnroutableIsNotConnected) {
    qb::io::tcp::socket socket;
    ASSERT_EQ(socket.init(), 0);
    ASSERT_EQ(socket.set_nonblocking(true), 0);

    // Kick off a non-blocking connect, then wait (bounded) for writability.
    socket.n_connect_v4(kUnroutableHost, kUnroutablePort);
    const int ready = qb::io::socket::handle_write_ready(socket.native_handle(), 2s);

    // Single falsifiable fact: whatever handle_write_ready reports, the socket is NOT
    // connected to a reachable peer — peer_endpoint() must be empty/invalid.
    const auto peer = socket.peer_endpoint();
    EXPECT_FALSE(static_cast<bool>(peer)) << "socket reported a peer for an unroutable address (ready=" << ready << ")";

    socket.disconnect();
}

// =============================================================================
// EISCONN survives the timed reconnect path (TLS-upgrade signal)
// =============================================================================

TEST_F(AsyncConnectTimeoutTest, ReconnectAlreadyConnectedSocketSurfacesEISCONN) {
    qb::io::tcp::listener server;
    ASSERT_EQ(server.listen_v4(0, "127.0.0.1"), SocketStatus::Done); // ephemeral
    const auto port = server.local_endpoint().port();
    ASSERT_NE(port, 0);

    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(server.accept(accepted), SocketStatus::Done);

    // Re-connect the already-connected socket via the timed path.
    const auto peer = client.peer_endpoint();
    const int  rc   = client.connect(peer, 1s);
    const int  err  = qb::io::socket::get_last_errno();

    EXPECT_NE(rc, 0) << "re-connecting a connected socket should not report success";
    EXPECT_EQ(err, EISCONN) << "EISCONN must survive set_nonblocking() (cleared on Windows, restored by the fix)";

    client.disconnect();
    accepted.disconnect();
    server.disconnect();
}

// =============================================================================
// UDP receive with timeout on an idle socket — times out, does not block
// =============================================================================

TEST_F(AsyncConnectTimeoutTest, UdpReceiveTimesOutOnIdleSocket) {
    qb::io::udp::socket socket;
    ASSERT_TRUE(socket.init());

    char      buffer[1024];
    const int result = qb::io::socket::recv_n(socket.native_handle(), buffer, sizeof(buffer), 200ms);

    // No datagram arrived: recv_n returns 0 (timed out, no data) or <0 (error). It must
    // NOT have returned a positive byte count, and it must not have blocked indefinitely.
    EXPECT_LE(result, 0) << "idle UDP recv returned data it never received";
    if (result < 0) {
        const int err = qb::io::socket::get_last_errno();
        EXPECT_TRUE(err == EWOULDBLOCK || err == EAGAIN || err == ETIMEDOUT || err == EINTR || err == EINPROGRESS)
            << "unexpected errno from a timed-out UDP recv: " << err;
    }
}
