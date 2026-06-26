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
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

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
