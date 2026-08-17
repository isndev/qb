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
std::atomic<int>  g_eos_count{0};

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
        *this << "unsent" << Protocol::end; // queued, then abandoned by the abort below
        disconnect(0);
    }

    // The other side of the eos scoping. An aborting disconnect() has NOT delivered what it
    // was holding, so it must stay silent — the eos emission added for close_after_deliver()
    // is deliberately gated on `!_reason`, and this is what pins that gate.
    void
    on(qb::io::async::event::eos &&) {
        g_eos_count.fetch_add(1);
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
    g_eos_count.store(0);

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
    EXPECT_EQ(g_eos_count.load(), 0) << "an aborting disconnect() delivered nothing, so it must not report eos";
}

// =============================================================================
// close_after_deliver() flushes pending data then disconnects
// =============================================================================

namespace cad {

std::atomic<bool> g_destroyed{false};
// `eos` means "everything queued has reached the transport". close_after_deliver() is the
// one path that asks the framework to wait for exactly that before closing, so it is the
// path where a handler most wants the notification — and it was the one path that never
// emitted it: handle_write() tested `!_protocol->ok()` (which close_after_deliver() sets)
// and returned before the eos block. The write-only sibling `async::output::on(event::io)`
// emits eos on every drain with no protocol test, so the two write paths disagreed.
std::atomic<int>  g_eos_count{0};
std::atomic<bool> g_eos_before_destroy{false};

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

    // Defining this handler is what opts the session in: the emission is guarded by
    // `if constexpr (qb::has_on<_Derived, event::eos>)`.
    void
    on(qb::io::async::event::eos &&) {
        g_eos_count.fetch_add(1);
        if (!g_destroyed.load())
            g_eos_before_destroy.store(true);
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
    g_eos_count.store(0);
    g_eos_before_destroy.store(false);

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

    // The eos contract: `event::eos` is documented as "all buffered data has been written
    // and sent", unconditionally, and close_after_deliver() is precisely a request to be
    // told when that has happened. Before the fix this read 0 — the graceful-close branch
    // returned before the emission, so the one case that asked for the notification was
    // the one case that never got it.
    EXPECT_EQ(g_eos_count.load(), 1) << "close_after_deliver() must still report final delivery via eos";
    EXPECT_TRUE(g_eos_before_destroy.load()) << "eos must be delivered while the session is alive, before disposal";
}

// =============================================================================
// io_handler::stream() broadcasts to every live session
// =============================================================================

namespace bc {

constexpr int kNumClients = 3;

// Client-side witnesses. The server cannot see what a client received, and the broadcast is
// server->client, so nothing observable on the server proves the fan-out arrived: these count it
// where it actually happens.
std::atomic<int> g_clients_got_hello{0};   // clients that read "hello_all" back off the wire
std::atomic<int> g_conns_at_broadcast{-1}; // sessions registered when the trigger was handled

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
    int  broadcast_fanout = 0; // live sessions stream() was fanned out to, recorded AT the broadcast
    bool all_received     = false;

    void
    on(IOSession &) {
        ++connection_count;
    }

    void
    broadcast_to_all(std::string_view msg) {
        // `sessions()` IS the set stream() writes to, so sampling it here (not later, after clients
        // start dropping) is the server-side half of the claim and the only part the server can know.
        broadcast_fanout = static_cast<int>(this->sessions().size());
        g_conns_at_broadcast.store(connection_count);
        this->stream(msg, '\n');
    }

    /**
     * The condition the test pumps on.
     *
     * It used to walk `sessions()` requiring `message_count >= 2` each. A server-side session only
     * ever counts messages the CLIENT sent it, and each client sends at most one, so `>= 2` was
     * unreachable for every session — the loop could succeed only over an EMPTY map. Measured on the
     * pre-fix binary: `all_received` became true with 0 live sessions, i.e. once all three clients
     * had disconnected. It was a vacuous quantifier, never a statement about the broadcast, and it
     * would have held just as well had `stream()` sent nothing at all.
     *
     * What the test's name claims is that the fan-out covered every live session and that every
     * client got the bytes. Both are now measured, so both are what this waits for.
     */
    void
    check_all_received() {
        all_received = broadcast_fanout >= kNumClients && g_clients_got_hello.load() >= kNumClients;
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
    g_clients_got_hello.store(0);
    g_conns_at_broadcast.store(-1);

    BroadcastServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    constexpr int            num_clients = kNumClients;
    std::vector<std::thread> clients;
    for (int c = 0; c < num_clients; ++c) {
        clients.emplace_back([c, port]() {
            qb::io::tcp::socket sock;
            ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
            if (c == 0) {
                std::this_thread::sleep_for(100ms); // let the other two register first
                sock.write("broadcast_trigger\n", 18);
            }
            // Deadline-bounded, and it stops on the payload it came for. A blocking read() returns
            // on ANY byte and never returns at all when the broadcast is not sent — which made
            // `t.join()` below the place this case hung on its own failure, after pump_until had
            // already reported it. Same wire traffic, but a client that is never served now ends.
            sock.set_nonblocking(true);
            char        buffer[512]{};
            std::size_t total    = 0;
            const auto  deadline = std::chrono::steady_clock::now() + 5s;
            while (total < sizeof(buffer) && std::chrono::steady_clock::now() < deadline) {
                const auto n = sock.read(buffer + total, sizeof(buffer) - total);
                if (n > 0) {
                    total += static_cast<std::size_t>(n);
                    if (std::string_view(buffer, total).find("hello_all") != std::string_view::npos) {
                        g_clients_got_hello.fetch_add(1);
                        break;
                    }
                } else {
                    std::this_thread::sleep_for(5ms);
                }
            }
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
    // The 100 ms sleep above only makes the intended interleaving LIKELY; this is what makes it
    // checked. On the inverted timing the trigger is handled with fewer sessions registered, the
    // broadcast reaches fewer clients, and the case used to pass regardless.
    EXPECT_EQ(g_conns_at_broadcast.load(), num_clients)
        << "the trigger was handled before every peer had registered — the broadcast could not have reached them all";
    EXPECT_EQ(server.broadcast_fanout, num_clients) << "stream() fanned out to fewer than every live session";
    EXPECT_EQ(g_clients_got_hello.load(), num_clients) << "not every client read the broadcast payload back";
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

// =============================================================================
// registerSession's construction order: the session exists BEFORE it owns a socket
//
// The pair here is "the session constructor" vs "the server's on(IOSession&) hook".
// io_handler::registerSession() must build the session before it can move the accepted
// socket into it, so anything touching the descriptor from a session CONSTRUCTOR
// addresses fd -1 and silently fails — set_optval (TCP_NODELAY, SO_*), getsockname, peer
// lookup, TLS handle access. Nothing reports it, which is why it cost an example program
// a debugging session. The hook is the place that works, and this pins both halves so the
// order stays a documented contract rather than an implementation detail.
// =============================================================================

namespace ord {

std::atomic<bool> g_ctor_saw_open_socket{true}; // pessimistic: proven false below
std::atomic<bool> g_hook_saw_open_socket{false};
std::atomic<bool> g_hook_ran{false};
std::atomic<int>  g_ctor_setopt_result{0};

class OrderServer;

class OrderSession : public use<OrderSession>::tcp::client<OrderServer> {
public:
    using Protocol = qb::protocol::text::command<OrderSession>;

    explicit OrderSession(IOServer &server)
        : client(server) {
        // Step 1 of registerSession: the transport is default-constructed here.
        g_ctor_saw_open_socket.store(transport().is_open());
        // The exact call an example tried to make. It "succeeds" as far as the caller can
        // tell -- there is no exception and the return is discarded by most callers -- but
        // it is addressing an invalid descriptor.
        g_ctor_setopt_result.store(transport().set_optval(IPPROTO_TCP, TCP_NODELAY, 1));
    }

    void
    on(Protocol::message &&) {}
};

class OrderServer : public use<OrderServer>::tcp::server<OrderSession> {
public:
    // Step 4 of registerSession, after `session->transport() = std::move(new_io)`.
    void
    on(IOSession &s) {
        g_hook_saw_open_socket.store(s.transport().is_open());
        g_hook_ran.store(true);
    }
};

} // namespace ord

TEST_F(SessionRoundtripTest, SessionConstructorHasNoSocketButTheHookDoes) {
    using namespace ord;
    g_ctor_saw_open_socket.store(true);
    g_hook_saw_open_socket.store(false);
    g_hook_ran.store(false);
    g_ctor_setopt_result.store(0);

    OrderServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::thread t([port]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
        sock.write("hello\n", 6);
        std::this_thread::sleep_for(100ms);
        sock.disconnect();
    });

    EXPECT_TRUE(pump_until([] { return g_hook_ran.load(); }, 5s)) << "the server's on(IOSession&) hook never ran";
    t.join();

    EXPECT_FALSE(g_ctor_saw_open_socket.load()) << "the session constructor must NOT be assumed to own a socket";
    EXPECT_NE(g_ctor_setopt_result.load(), 0) << "set_optval from a constructor addresses fd -1 and fails silently";
    EXPECT_TRUE(g_hook_saw_open_socket.load()) << "on(IOSession&) is the hook that runs with the socket installed";
}
