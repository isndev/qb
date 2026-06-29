/**
 * @file system/session/text-protocol-roundtrip.cpp
 * @brief Full `use<>::tcp::client/server` + `qb::protocol::text` session lifecycle over loopback.
 *
 * These exercise the high-level session/protocol stack end-to-end on real loopback TCP sockets driven
 * by the in-process libev loop, so they are SYSTEM tests (network). The subject is the framed text
 * protocol (`qb::protocol::text::command`) carried over the `use<T>::tcp::client`/`server` CRTP
 * scaffolding, plus the session-control primitives the server side calls on a live connection:
 *
 *   - line-framed request/response echo, N iterations both directions, with EXACT per-message length
 *     and final message-count assertions made in the TEST BODY (not in session destructors, where a
 *     leak or hang would silently skip them);
 *   - `disconnect(0)` triggers real session disposal (the session object is destroyed);
 *   - `close_after_deliver()` flushes the pending write THEN disconnects (the peer sees the bytes);
 *   - `io_handler::stream()` fan-out broadcasts to every live session;
 *   - N rapid connect-then-disconnect clients each produce EXACTLY one server-side `disconnected`
 *     event (tightened from the old smoke `GE(...,1)` to `EQ(...,N)`).
 *
 * Restructured from the dissolved system/test-async-io.cpp (TextProtocolCommunication,
 * DisconnectZeroTriggersDisposal, CloseAfterDeliverSendsDataThenDisconnects, BroadcastToMultipleSessions,
 * MultipleRapidDisconnects). Per the restructure spec: every fixed port becomes ephemeral
 * `listen_v4(0)` + `local_endpoint().port()`; the destructor-side `EXPECT_EQ(msg_count_*, …)` move into
 * the test body; the hand-rolled poll loops on the SERVER loop are replaced by `qb::io::test::pump_until`;
 * `MultipleRapidDisconnects` asserts the full count. File-global counters are replaced by per-test state
 * reached through pointers. No file-local main().
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
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

constexpr const char        TEXT_MESSAGE[]  = "Hello, Text Protocol!";
constexpr const std::size_t TEXT_ITERATIONS = 10;

class SessionRoundtripTest : public ::testing::Test {
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
// Text protocol echo round-trip (server echoes; client tallies)
// =============================================================================

namespace text_rt {

std::atomic<std::size_t> g_msg_server{0};
std::atomic<std::size_t> g_msg_client{0};
std::atomic<std::size_t> g_server_connections{0};

class TextServer;

class TextServerClient : public use<TextServerClient>::tcp::client<TextServer> {
public:
    using Protocol = qb::protocol::text::command<TextServerClient>;

    explicit TextServerClient(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        *this << msg.text << Protocol::end; // echo
        g_msg_server.fetch_add(1);
    }
};

class TextServer : public use<TextServer>::tcp::server<TextServerClient> {
public:
    void
    on(IOSession &) {
        g_server_connections.fetch_add(1);
    }
};

class TextClient : public use<TextClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<TextClient>;

    void
    on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        g_msg_client.fetch_add(1);
    }
};

} // namespace text_rt

TEST_F(SessionRoundtripTest, TextProtocolEchoRoundTrip) {
    using namespace text_rt;
    g_msg_server.store(0);
    g_msg_client.store(0);
    g_server_connections.store(0);

    TextServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread client_thread([port]() {
        async::init();
        TextClient client;
        ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), SocketStatus::Done);
        client.start();

        for (std::size_t i = 0; i < TEXT_ITERATIONS; ++i)
            client << TEXT_MESSAGE << '\n';

        // Pump the client loop until both directions have tallied all messages.
        // (Discard the result here — the server-side EXPECT below is the authoritative check.)
        (void) qb::io::test::pump_until([] { return g_msg_server.load() >= TEXT_ITERATIONS && g_msg_client.load() >= TEXT_ITERATIONS; }, 5s);
    });

    // Pump the server loop to the same completion condition.
    EXPECT_TRUE(pump_until([] { return g_msg_server.load() >= TEXT_ITERATIONS && g_msg_client.load() >= TEXT_ITERATIONS; }, 5s))
        << "text protocol round-trip did not complete";

    client_thread.join();

    // Assertions in the TEST BODY (always run, unlike the old destructor-side checks).
    EXPECT_EQ(g_msg_server.load(), TEXT_ITERATIONS);
    EXPECT_EQ(g_msg_client.load(), TEXT_ITERATIONS);
    EXPECT_EQ(g_server_connections.load(), 1u);
}

// =============================================================================
// disconnect(0) triggers session disposal
// =============================================================================

namespace dz {

std::atomic<bool> g_destroyed{false};
std::atomic<bool> g_connected{false};

class DisconnectZeroServer;

class DisconnectZeroSession : public use<DisconnectZeroSession>::tcp::client<DisconnectZeroServer> {
public:
    using Protocol = qb::protocol::text::command<DisconnectZeroSession>;

    explicit DisconnectZeroSession(IOServer &server)
        : client(server) {}

    ~DisconnectZeroSession() {
        g_destroyed.store(true);
    }

    void
    on(Protocol::message &&) {
        disconnect(0);
    }
};

class DisconnectZeroServer : public use<DisconnectZeroServer>::tcp::server<DisconnectZeroSession> {
public:
    void
    on(IOSession &) {
        g_connected.store(true);
    }
};

} // namespace dz

TEST_F(SessionRoundtripTest, DisconnectZeroTriggersDisposal) {
    using namespace dz;
    g_destroyed.store(false);
    g_connected.store(false);

    DisconnectZeroServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread t([port]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
        sock.write("hello\n", 6);
        std::this_thread::sleep_for(200ms);
        sock.disconnect();
    });

    EXPECT_TRUE(pump_until([] { return g_destroyed.load(); }, 5s)) << "disconnect(0) did not dispose the session";
    t.join();

    EXPECT_TRUE(g_connected.load());
    EXPECT_TRUE(g_destroyed.load());
}

// =============================================================================
// close_after_deliver() flushes pending data then disconnects
// =============================================================================

namespace cad {

std::atomic<bool> g_destroyed{false};

class CloseAfterDeliverServer;

class CloseAfterDeliverSession : public use<CloseAfterDeliverSession>::tcp::client<CloseAfterDeliverServer> {
public:
    using Protocol = qb::protocol::text::command<CloseAfterDeliverSession>;

    explicit CloseAfterDeliverSession(IOServer &server)
        : client(server) {}

    ~CloseAfterDeliverSession() {
        g_destroyed.store(true);
    }

    void
    on(Protocol::message &&) {
        *this << "goodbye" << Protocol::end;
        close_after_deliver();
    }
};

class CloseAfterDeliverServer : public use<CloseAfterDeliverServer>::tcp::server<CloseAfterDeliverSession> {
public:
    void
    on(IOSession &) {}
};

} // namespace cad

TEST_F(SessionRoundtripTest, CloseAfterDeliverSendsDataThenDisconnects) {
    using namespace cad;
    g_destroyed.store(false);

    CloseAfterDeliverServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::atomic<bool> got_goodbye{false};
    std::atomic<bool> client_done{false};
    std::thread       t([&got_goodbye, &client_done, port]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
        sock.write("ping\n", 5);

        char        buffer[512]{};
        std::size_t total    = 0;
        const auto  deadline = std::chrono::steady_clock::now() + 3s;
        sock.set_nonblocking(true);
        while (total == 0 && std::chrono::steady_clock::now() < deadline) {
            const auto n = sock.read(buffer + total, sizeof(buffer) - total);
            if (n > 0)
                total += static_cast<std::size_t>(n);
            else
                std::this_thread::sleep_for(10ms);
        }
        if (total > 0 && std::string_view(buffer, total).find("goodbye") != std::string_view::npos)
            got_goodbye.store(true);
        sock.disconnect();
        client_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return g_destroyed.load() && client_done.load(); }, 5s)) << "close_after_deliver did not flush-then-dispose";
    t.join();

    EXPECT_TRUE(got_goodbye.load()) << "the pending 'goodbye' was not delivered before close";
    EXPECT_TRUE(g_destroyed.load());
}

// =============================================================================
// io_handler::stream() broadcasts to every live session
// =============================================================================

namespace bc {

class BroadcastServer;

class BroadcastSession : public use<BroadcastSession>::tcp::client<BroadcastServer> {
public:
    using Protocol    = qb::protocol::text::command<BroadcastSession>;
    int message_count = 0;

    explicit BroadcastSession(IOServer &server)
        : client(server) {}

    void on(Protocol::message &&msg);
};

class BroadcastServer : public use<BroadcastServer>::tcp::server<BroadcastSession> {
public:
    int  connection_count = 0;
    bool all_received     = false;

    void
    on(IOSession &) {
        ++connection_count;
    }

    void
    broadcast_to_all(std::string_view msg) {
        this->stream(msg, '\n');
    }

    void
    check_all_received() {
        if (connection_count < 3)
            return;
        for (auto &[id, session] : this->sessions())
            if (session->message_count < 2)
                return;
        all_received = true;
    }
};

void
BroadcastSession::on(Protocol::message &&msg) {
    ++message_count;
    if (msg.text == "broadcast_trigger")
        static_cast<BroadcastServer &>(this->server()).broadcast_to_all("hello_all");
}

} // namespace bc

TEST_F(SessionRoundtripTest, BroadcastReachesAllSessions) {
    using namespace bc;
    BroadcastServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int            num_clients = 3;
    std::vector<std::thread> clients;
    for (int c = 0; c < num_clients; ++c) {
        clients.emplace_back([c, port]() {
            qb::io::tcp::socket sock;
            ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
            if (c == 0) {
                std::this_thread::sleep_for(100ms); // let the other two register first
                sock.write("broadcast_trigger\n", 18);
            }
            sock.set_nonblocking(false);
            char buffer[512]{};
            sock.read(buffer, sizeof(buffer));
            sock.disconnect();
        });
    }

    EXPECT_TRUE(pump_until(
        [&] {
            server.check_all_received();
            return server.all_received;
        },
        5s))
        << "broadcast did not reach all sessions";

    for (auto &t : clients)
        t.join();

    EXPECT_EQ(server.connection_count, num_clients);
}

// =============================================================================
// Rapid connect/disconnect — EVERY client yields one disconnected event
// =============================================================================

namespace rapid {

std::atomic<int> g_disconnects{0};

class RapidServer;

class RapidSession : public use<RapidSession>::tcp::client<RapidServer> {
public:
    using Protocol = qb::protocol::text::command<RapidSession>;
    explicit RapidSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}
    void
    on(async::event::disconnected const &) {
        g_disconnects.fetch_add(1);
    }
};

class RapidServer : public use<RapidServer>::tcp::server<RapidSession> {
public:
    void
    on(IOSession &) {}
};

} // namespace rapid

TEST_F(SessionRoundtripTest, RapidDisconnectsAllObserved) {
    using namespace rapid;
    g_disconnects.store(0);

    RapidServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int            kClients = 10;
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port]() {
            qb::io::tcp::socket sock;
            if (sock.connect_v4("127.0.0.1", port) == SocketStatus::Done) {
                sock.write("hello\n", 6);
                sock.disconnect();
            }
        });
    }

    // Tightened from GE(...,1) to EQ(...,kClients): the server must observe a
    // disconnected event for EVERY client, not merely "at least one".
    EXPECT_TRUE(pump_until([] { return g_disconnects.load() == kClients; }, 5s))
        << "only " << g_disconnects.load() << "/" << kClients << " disconnects were observed";

    for (auto &t : clients)
        t.join();

    EXPECT_EQ(g_disconnects.load(), kClients);
}
