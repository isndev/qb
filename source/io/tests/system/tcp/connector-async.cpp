/**
 * @file system/tcp/connector-async.cpp
 * @brief Successful `qb::io::async::tcp::connect` over loopback — fresh and moved-in sockets.
 *
 * The async connector (`qb::io::async::tcp::connect`, qb/io/async/tcp/connector.h) drives a
 * non-blocking connect to completion on the in-process libev loop and hands the caller an OPEN,
 * usable socket via the completion callback. These are SYSTEM tests (network): a real loopback
 * server thread accepts and echoes one byte so the connected socket is proven end-to-end usable.
 *
 * Contracts proven (the connector's happy path — its failure/timeout semantics live in
 * system/async/async-connect-timeout.cpp and are not duplicated here):
 *   - the default overload connects a FRESH socket; the callback receives an open socket that can
 *     write a byte and read the server's echo back;
 *   - the move-in overload (`connect(std::move(existing), …)`) reuses a caller-provided socket and is
 *     equally usable end-to-end;
 *   - the completion callback is delivered from the loop (the caller pumps it), never synchronously.
 *
 * Restructured from the dissolved system/test-async-io.cpp (ConnectorSucceedsWithFreshAndExistingSocket
 * — and the success-half framing of the connector group). Ports are ephemeral via the shared
 * `qb::io::test::reserve_free_tcp_port`-style loopback (here a local listener bound to `:0`); the
 * hand-rolled poll loops become `qb::io::test::pump_until`. No file-local main().
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
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/tcp/acceptor.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/transport/accept.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/loopback_fixture.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;
using qb::io::test::reserve_free_tcp_port;

namespace {

class ConnectorAsyncTest : public ::testing::Test {
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

// Accept `count` connections, echo one byte on each, then disconnect. Runs on a
// dedicated thread so the calling thread can pump the connector's loop.
void
echo_one_byte_server(qb::io::tcp::listener &listener, int count) {
    for (int i = 0; i < count; ++i) {
        qb::io::tcp::socket accepted;
        ASSERT_EQ(listener.accept(accepted), SocketStatus::Done);
        accepted.set_nonblocking(false);
        char marker = 0;
        ASSERT_EQ(accepted.read(&marker, sizeof(marker)), 1);
        ASSERT_EQ(accepted.write(&marker, sizeof(marker)), 1);
        accepted.disconnect();
    }
}

// A minimal raw acceptor driving the `qb::io::transport::accept` transport
// directly (the same _Prot base that `use<>::tcp::server` builds on, but without
// the io_handler/session layer on top). It listens on an ephemeral loopback port,
// and on each accepted connection records the fd and bumps a counter. Its sole
// purpose is to exercise the accept-transport's accept→getAccepted→flush hand-off
// and its `close()` path from a SYSTEM test, deterministically over loopback.
struct RawAcceptorServer : qb::io::async::tcp::acceptor<RawAcceptorServer, qb::io::transport::accept> {
    std::atomic<int> *accepted_count = nullptr;
    int               last_handle    = -1;

    void
    on(qb::io::tcp::socket &&accepted) {
        last_handle = accepted.native_handle();
        if (accepted_count)
            accepted_count->fetch_add(1);
        accepted.disconnect();
    }
};

} // namespace

// =============================================================================
// Fresh-socket connect: open + usable end-to-end
// =============================================================================

TEST_F(ConnectorAsyncTest, FreshSocketConnectsAndEchoesByte) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server([&] { echo_one_byte_server(listener, 1); });

    std::atomic<bool> done{false};
    bool              connected = false;
    bool              echoed    = false;

    async::tcp::connect<qb::io::tcp::socket>(
        uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](qb::io::tcp::socket &&sock) {
            connected = sock.is_open();
            if (sock.is_open()) {
                ASSERT_EQ(sock.write("a", 1), 1);
                char reply = 0;
                sock.set_nonblocking(false);
                echoed = (sock.read(&reply, sizeof(reply)) == 1) && (reply == 'a');
                sock.disconnect();
            }
            done = true;
        },
        1s);

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "fresh-socket connect callback never delivered";
    EXPECT_TRUE(connected) << "fresh socket connect did not yield an open socket";
    EXPECT_TRUE(echoed) << "connected fresh socket did not round-trip the echo byte";

    server.join();
}

// =============================================================================
// Move-in connect: a caller-provided socket is reused + usable end-to-end
// =============================================================================

TEST_F(ConnectorAsyncTest, MovedInSocketConnectsAndEchoesByte) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server([&] { echo_one_byte_server(listener, 1); });

    std::atomic<bool>   done{false};
    bool                connected = false;
    bool                echoed    = false;
    qb::io::tcp::socket existing; // caller-provided, default-constructed

    async::tcp::connect<qb::io::tcp::socket>(
        std::move(existing), uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](qb::io::tcp::socket &&sock) {
            connected = sock.is_open();
            if (sock.is_open()) {
                ASSERT_EQ(sock.write("b", 1), 1);
                char reply = 0;
                sock.set_nonblocking(false);
                echoed = (sock.read(&reply, sizeof(reply)) == 1) && (reply == 'b');
                sock.disconnect();
            }
            done = true;
        },
        1s);

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "moved-in-socket connect callback never delivered";
    EXPECT_TRUE(connected) << "moved-in socket connect did not yield an open socket";
    EXPECT_TRUE(echoed) << "connected moved-in socket did not round-trip the echo byte";

    server.join();
}

// =============================================================================
// Failure path: a connect to a port with no listener completes through the
// connector's failure machinery and hands the caller a CLOSED socket.
//
// The success cases above only walk `finalize_transport_connect()==done` and
// `deliver(std::move(socket_))`. This case drives the other tail of the
// connector: an unconnectable loopback target makes the non-blocking connect
// fail (synchronously as ECONNREFUSED, or asynchronously via the writable
// event with SO_ERROR set), which routes through `deliver_failure_deferred()`
// / the `on()` error branch and `deliver(Socket_{})`. The callback must still
// be delivered exactly once, from the loop, with a NON-open socket. No server
// thread is needed — the whole point is that nothing is listening.
//
// The target port is reserved via the shared loopback helper (bind :0, read
// the kernel assignment, close) so it is almost certainly unbound for the
// brief connect window — and on loopback a connect to an unbound port refuses
// immediately rather than hanging, keeping this deterministic under a bounded
// pump. A generous 2s timeout is supplied purely as a backstop; the refusal,
// not the deadline, is what completes the connect.
// =============================================================================

TEST_F(ConnectorAsyncTest, ConnectToUnlistenedPortDeliversClosedSocket) {
    const auto port = reserve_free_tcp_port();
    ASSERT_NE(port, 0);

    std::atomic<bool> done{false};
    bool              callback_socket_open = true; // must be flipped to false
    int               callbacks            = 0;

    async::tcp::connect<qb::io::tcp::socket>(
        uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](qb::io::tcp::socket &&sock) {
            ++callbacks;
            callback_socket_open = sock.is_open();
            done                 = true;
        },
        2s);

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "failed-connect callback never delivered";
    EXPECT_EQ(callbacks, 1) << "connector must deliver its completion callback exactly once";
    EXPECT_FALSE(callback_socket_open) << "a refused connect must hand back a closed socket";
}

// =============================================================================
// No-deadline connect: a `timeout == 0` connect drives the connector's no-deadline
// branch (`arm_deadline()` early-returns because `deadline_ <= 0`), while the EV_WRITE
// completion still delivers an open, usable socket. The success cases above all pass a
// positive timeout (so the deadline is armed); this one proves the zero-timeout path —
// the connector's documented "0 = wait indefinitely for writability" contract —
// completes purely on writability with no deadline timer in play.
// =============================================================================

TEST_F(ConnectorAsyncTest, FreshSocketConnectsWithoutDeadlineAndEchoesByte) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::thread server([&] { echo_one_byte_server(listener, 1); });

    std::atomic<bool> done{false};
    bool              connected = false;
    bool              echoed    = false;

    // No timeout argument -> qb::duration::zero() -> deadline_ == 0 -> arm_deadline()
    // takes its `deadline_ <= 0.` early return; only the EV_WRITE completion drives it.
    async::tcp::connect<qb::io::tcp::socket>(
        uri{"tcp://127.0.0.1:" + std::to_string(port)},
        [&](qb::io::tcp::socket &&sock) {
            connected = sock.is_open();
            if (sock.is_open()) {
                ASSERT_EQ(sock.write("z", 1), 1);
                char reply = 0;
                sock.set_nonblocking(false);
                echoed = (sock.read(&reply, sizeof(reply)) == 1) && (reply == 'z');
                sock.disconnect();
            }
            done = true;
        });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "no-deadline connect callback never delivered";
    EXPECT_TRUE(connected) << "no-deadline connect did not yield an open socket";
    EXPECT_TRUE(echoed) << "no-deadline connected socket did not round-trip the echo byte";

    server.join();
}

// =============================================================================
// Accept transport (qb::io::transport::accept): the async acceptor's accept loop.
//
// A raw `acceptor<_, transport::accept>` listens on an ephemeral loopback port and
// auto-starts its accept watcher via `listen()`. A real client connects; the loop's
// readiness drives `transport::accept::read()` to the `SocketStatus::Done` branch,
// which returns the accepted fd, then the accept protocol's `onMessage()` moves the
// socket out via `getAccepted()` (and `flush()` releases its handle so the move owns
// it). We then explicitly close the listener through the accept transport's `close()`
// to cover the listener-teardown path. All bounded by pump_until — a stall fails loud.
// =============================================================================

TEST_F(ConnectorAsyncTest, AcceptTransportAcceptsClientAndClosesListener) {
    std::atomic<int>  accepted_count{0};
    RawAcceptorServer server;
    server.accepted_count = &accepted_count;

    // Bind an ephemeral loopback port directly on the accept transport's listener,
    // then start the accept watcher (the same two-step the server wrappers do; uses
    // listen_v4(0) per the restructure spec's no-fixed-ports invariant).
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done) << "acceptor failed to bind an ephemeral loopback port";
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), SocketStatus::Done);

    EXPECT_TRUE(pump_until([&] { return accepted_count.load() == 1; })) << "accept transport never accepted the loopback client";
    EXPECT_EQ(accepted_count.load(), 1);
    EXPECT_GE(server.last_handle, 0) << "accepted socket should carry a valid native handle";

    client.disconnect();

    // Drive transport::accept::close() (listener teardown) explicitly through the
    // accept base, then confirm the listener is no longer accepting.
    static_cast<qb::io::transport::accept &>(server).close();
    EXPECT_FALSE(server.transport().is_open()) << "accept transport close() must shut the listening socket";
}
