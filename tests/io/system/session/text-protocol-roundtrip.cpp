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

// The peer is still sending when the session answers and asks to close: what it sent after the
// answer is never read, so the server closes a socket with unread input -- which the TCP stack turns
// into a reset. The reset fails the peer's send everywhere, and on Windows it also destroys the
// answer the peer received and has not read yet. The client below sends its line, then a large tail,
// and reads only once its upload is out: the deterministic shape of that race (Huly QB-1089).
TEST_F(SessionRoundtripTest, CloseAfterDeliverReachesAPeerStillSending) {
    using namespace cad;
    g_destroyed.store(false);
    g_eos_count.store(0);
    g_eos_before_destroy.store(false);

    CloseAfterDeliverServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    std::atomic<bool> client_done{false};
    std::string       received;
    int               read_error  = 0;
    std::size_t       upload_left = 0;
    int               write_error = 0;
    std::thread       t([&received, &read_error, &upload_left, &write_error, &client_done, port]() {
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.connect_v4("127.0.0.1", port), SocketStatus::Done);
        sock.set_nonblocking(true);
        std::string       pending = "ping\n";
        const std::string tail(4 * 1024 * 1024, 'x');
        pending += tail;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
            const int n = sock.write(pending.data(), pending.size());
            if (n > 0)
                pending.erase(0, static_cast<std::size_t>(n));
            else if (n < 0 && !qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
                write_error = qb::io::socket::get_last_errno();
                break; // reset by the server
            } else
                std::this_thread::sleep_for(1ms);
        }
        upload_left = pending.size();
        char buffer[512];
        while (std::chrono::steady_clock::now() < deadline) {
            const int n     = sock.read(buffer, sizeof(buffer));
            const int error = n < 0 ? qb::io::socket::get_last_errno() : 0;
            if (n > 0)
                received.append(buffer, static_cast<std::size_t>(n));
            else if (error == 0) // tcp::socket::read() reports EOF as -1 with no error
                break;
            else if (!qb::io::socket::not_recv_error(error)) {
                read_error = error;
                break;
            } else
                std::this_thread::sleep_for(1ms);
        }
        sock.disconnect();
        client_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return g_destroyed.load() && client_done.load(); }, 10s));
    t.join();
    EXPECT_NE(received.find("goodbye"), std::string::npos)
        << "the answer did not reach a peer that was still sending (read error " << read_error << ")";
    EXPECT_EQ(upload_left, 0u) << "the peer was reset while it was still sending (write error " << write_error << ")";
}

// =============================================================================
// The lingering close of a server session (Huly QB-1089): its life cycle
// =============================================================================

namespace linger_test {

using linger_clock = std::chrono::steady_clock;

std::atomic<bool>        g_destroyed{false};
std::atomic<bool>        g_delivered{false};
std::atomic<int>         g_disconnected{0};
qb::duration             g_linger = 2s;
linger_clock::time_point g_delivered_at{};
linger_clock::time_point g_destroyed_at{};

class LingerServer;

class LingerSession : public use<LingerSession>::tcp::client<LingerServer> {
public:
    using Protocol = qb::protocol::text::command<LingerSession>;

    explicit LingerSession(IOServer &server)
        : client(server) {}

    ~LingerSession() {
        g_destroyed_at = linger_clock::now();
        g_destroyed.store(true);
    }

    void
    on(Protocol::message &&) {
        *this << "goodbye" << Protocol::end;
        close_after_deliver();
    }

    void
    on(qb::io::async::event::eos &&) {
        g_delivered_at = linger_clock::now();
        g_delivered.store(true);
    }

    void
    on(qb::io::async::event::disconnected &&) {
        g_disconnected.fetch_add(1);
    }

    // What a 3.3 session had to do itself. Since 3.4 `extractSession()` stops the watcher of the session it takes the
    // socket from, after this hook (ASessionHeldPastItsExtractionIsNotDispatchedAgain): with this stop() made first,
    // that one finds nothing left to stop.
    void
    on(qb::io::async::event::extracted &&) {
        stop();
    }
};

class LingerServer : public use<LingerServer>::tcp::server<LingerSession> {
public:
    void
    on(IOSession &session) {
        session.set_linger(g_linger);
    }
};

/// What the peer saw: the answer, then EOF -- the session's half-close -- or an error.
struct PeerView {
    std::string received;
    bool        eof   = false;
    int         error = 0;
};

/// The peer: sends "bye", reads until EOF or an error, then keeps its socket open until `release` is set,
/// so the test decides when -- and whether -- the peer closes.
void
run_peer(std::uint16_t port, PeerView &view, std::atomic<bool> &saw_end, std::atomic<bool> &release) {
    qb::io::tcp::socket sock;
    if (sock.connect_v4("127.0.0.1", port) != SocketStatus::Done) {
        saw_end.store(true);
        return;
    }
    sock.write("bye\n", 4);
    sock.set_nonblocking(true);
    char       buffer[256];
    const auto deadline = linger_clock::now() + 10s;
    while (linger_clock::now() < deadline) {
        const int n     = sock.read(buffer, sizeof(buffer));
        const int error = n < 0 ? qb::io::socket::get_last_errno() : 0;
        if (n > 0) {
            view.received.append(buffer, static_cast<std::size_t>(n));
        } else if (error == 0) {
            view.eof = true;
            break;
        } else if (!qb::io::socket::not_recv_error(error)) {
            view.error = error;
            break;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    saw_end.store(true);
    while (!release.load() && linger_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    sock.disconnect();
}

void
reset(qb::duration linger) {
    g_destroyed.store(false);
    g_delivered.store(false);
    g_disconnected.store(0);
    g_linger = linger;
}

[[nodiscard]] bool
any_lingering(LingerServer &server) {
    for (auto &[id, session] : server.sessions())
        if (session->lingering())
            return true;
    return false;
}

/// Runs the loop for `span`, whatever happens: no predicate, no failure.
void
pump_for(qb::duration span) {
    const auto until = linger_clock::now() + span;
    while (linger_clock::now() < until)
        async::run(EVRUN_NOWAIT);
}

class DeafLingerServer;

/// A session that answers, sets its read cap to 0 -- it reads nothing more -- and closes after delivery.
class DeafLingerSession : public use<DeafLingerSession>::tcp::client<DeafLingerServer> {
public:
    using Protocol = qb::protocol::text::command<DeafLingerSession>;

    explicit DeafLingerSession(IOServer &server)
        : client(server) {}

    ~DeafLingerSession() {
        g_destroyed_at = linger_clock::now();
        g_destroyed.store(true);
    }

    void
    on(Protocol::message &&) {
        *this << "goodbye" << Protocol::end;
        set_max_read_buffer_size(0u);
        close_after_deliver();
    }

    void
    on(qb::io::async::event::eos &&) {
        g_delivered_at = linger_clock::now();
        g_delivered.store(true);
    }

    void
    on(qb::io::async::event::disconnected &&) {
        g_disconnected.fetch_add(1);
    }
};

class DeafLingerServer : public use<DeafLingerServer>::tcp::server<DeafLingerSession> {
public:
    void
    on(IOSession &session) {
        session.set_linger(g_linger);
    }
};

} // namespace linger_test

// The answer is delivered, then the session waits for its peer: still registered, lingering, its sending
// side shut down -- the peer reads the answer then EOF -- and it ends as soon as the peer closes.
TEST_F(SessionRoundtripTest, ALingeringCloseWaitsForThePeerAndEndsWhenItCloses) {
    using namespace linger_test;
    reset(30s);
    LingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    ASSERT_TRUE(pump_until([&] { return g_delivered.load() && saw_end.load(); }, 5s));
    pump_for(20ms);
    EXPECT_TRUE(view.eof) << "the peer must read EOF once the answer is out: the sending side is shut down (error " << view.error << ")";
    EXPECT_NE(view.received.find("goodbye"), std::string::npos);
    EXPECT_FALSE(g_destroyed.load()) << "the session must wait for its peer";
    EXPECT_EQ(server.session_count(), 1u);
    EXPECT_TRUE(any_lingering(server));

    release.store(true);
    EXPECT_TRUE(pump_until([&] { return g_destroyed.load(); }, 5s)) << "the peer's close must end the linger at once, not after 30 s";
    peer.join();
    EXPECT_EQ(g_disconnected.load(), 1);
    EXPECT_EQ(server.session_count(), 0u);
}

// A peer that never closes is waited for linger(), no more.
TEST_F(SessionRoundtripTest, ALingeringCloseIsBoundedByItsLinger) {
    using namespace linger_test;
    reset(300ms);
    LingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    EXPECT_TRUE(pump_until([&] { return g_destroyed.load(); }, 5s)) << "the bound must end the linger";
    release.store(true);
    peer.join();
    ASSERT_TRUE(g_delivered.load());
    // Not before its bound -- the bound is what the linger is (a millisecond of timer granularity aside).
    EXPECT_GE(g_destroyed_at - g_delivered_at, 290ms);
    EXPECT_NE(view.received.find("goodbye"), std::string::npos);
    EXPECT_EQ(g_disconnected.load(), 1);
}

// A read cap of 0 drains nothing: the linger of such a session ends at the first octet its peer still sends, where it
// used to be dispatched for that octet on every pass of the loop until linger() elapsed -- here 30 s (Huly QB-86).
TEST_F(SessionRoundtripTest, ALingeringCloseThatCanDrainNothingEndsAtTheFirstOctet) {
    using namespace linger_test;
    reset(30s);
    DeafLingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    qb::io::tcp::socket peer;
    ASSERT_EQ(peer.connect_v4("127.0.0.1", port), SocketStatus::Done);
    ASSERT_EQ(peer.write("bye\n", 4u), 4);
    ASSERT_TRUE(pump_until([&] { return g_delivered.load(); }, 5s)) << "the answer was never delivered";
    pump_for(20ms);
    ASSERT_FALSE(g_destroyed.load()) << "nothing has arrived since the answer: the session waits for its peer";
    EXPECT_EQ(server.session_count(), 1u);

    ASSERT_EQ(peer.write("more\n", 5u), 5); // the peer is still sending
    EXPECT_TRUE(pump_until([&] { return g_destroyed.load(); }, 2s)) << "a linger that can drain nothing went on for its bound of 30 s";
    EXPECT_EQ(g_disconnected.load(), 1);
    EXPECT_EQ(server.session_count(), 0u);
    peer.close();
}

// set_linger(0): the session closes as soon as its answer is delivered, as before 3.3.
TEST_F(SessionRoundtripTest, AZeroLingerClosesAsSoonAsTheAnswerIsDelivered) {
    using namespace linger_test;
    reset(qb::duration::zero());
    LingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    EXPECT_TRUE(pump_until([&] { return g_destroyed.load(); }, 5s)) << "the peer has not closed: only an immediate close ends it";
    EXPECT_FALSE(any_lingering(server));
    release.store(true);
    peer.join();
    EXPECT_NE(view.received.find("goodbye"), std::string::npos);
}

// disconnect() is an abort: it ends a linger at the next dispatch.
TEST_F(SessionRoundtripTest, DisconnectAbortsALingeringClose) {
    using namespace linger_test;
    reset(30s);
    LingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    ASSERT_TRUE(pump_until([&] { return any_lingering(server); }, 5s));
    server.sessions().begin()->second->disconnect();
    EXPECT_TRUE(pump_until([&] { return g_destroyed.load(); }, 5s)) << "disconnect() must not wait for the linger";
    release.store(true);
    peer.join();
    EXPECT_EQ(g_disconnected.load(), 1);
}

// The server goes while one of its sessions lingers: no wait, no leak, and nothing fires afterwards.
TEST_F(SessionRoundtripTest, ServerTeardownDuringALingerNeitherWaitsNorLeaks) {
    using namespace linger_test;
    reset(30s);
    auto server = std::make_unique<LingerServer>();
    ASSERT_EQ(server->transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server->transport().local_endpoint().port();
    server->start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    ASSERT_TRUE(pump_until([&] { return any_lingering(*server); }, 5s));
    const auto before = linger_clock::now();
    server.reset();
    EXPECT_LT(linger_clock::now() - before, 1s) << "a lingering session must not hold its server's teardown";
    EXPECT_TRUE(g_destroyed.load());
    pump_for(100ms); // the session's bound went with it: nothing of it may run now
    release.store(true);
    peer.join();
}

// A lingering session whose socket is taken over (extractSession) leaves the linger: its bound must not
// end the session under its new owner.
TEST_F(SessionRoundtripTest, ExtractingALingeringSessionCancelsItsLinger) {
    using namespace linger_test;
    reset(100ms);
    LingerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    PeerView          view;
    std::atomic<bool> saw_end{false}, release{false};
    std::thread       peer([&] { run_peer(port, view, saw_end, release); });

    ASSERT_TRUE(pump_until([&] { return any_lingering(server); }, 5s));
    const auto id      = server.sessions().begin()->first;
    auto       session = server.session(id); // kept alive past the extraction
    auto [sock, ok]    = server.extractSession(id);
    ASSERT_TRUE(ok);
    EXPECT_FALSE(session->lingering());
    pump_for(400ms); // four bounds
    EXPECT_EQ(g_disconnected.load(), 0) << "the linger's bound ended a session that had left it";
    session.reset();
    sock.close();
    release.store(true);
    peer.join();
}

// =============================================================================
// A session whose transport was extracted is not dispatched again
// =============================================================================

namespace extracted_held {

int g_disconnected = 0; ///< `disconnected` events the session was handed
int g_messages     = 0; ///< messages it was handed

class HeldServer;

/// A session with NO `extracted` hook: nothing of its own stops its watcher when its transport is taken.
class HeldSession : public use<HeldSession>::tcp::client<HeldServer> {
public:
    using Protocol = qb::protocol::text::command<HeldSession>;

    explicit HeldSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {
        ++g_messages;
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++g_disconnected;
    }

    [[nodiscard]] bool
    watching() const noexcept {
        return this->_async_event.is_active();
    }
};

class HeldServer : public use<HeldServer>::tcp::server<HeldSession> {};

} // namespace extracted_held

// A session somebody still holds once its transport was taken (`extractSession()`: a `shared_ptr` kept across a
// protocol upgrade) watches nothing any more -- the descriptor is its new owner's. It kept its watcher on it: the peer's
// next octets dispatched the session, whose read of a transport it no longer has failed, and it was told it had
// disconnected, on a connection that goes on under its new owner.
TEST_F(SessionRoundtripTest, ASessionHeldPastItsExtractionIsNotDispatchedAgain) {
    using namespace extracted_held;
    g_disconnected = 0;
    g_messages     = 0;
    HeldServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    qb::io::tcp::socket peer;
    ASSERT_EQ(peer.connect_v4("127.0.0.1", port), SocketStatus::Done);
    ASSERT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 5s));
    const auto id      = server.sessions().begin()->first;
    auto       session = server.session(id); // kept alive past the extraction
    auto [sock, ok]    = server.extractSession(id);
    ASSERT_TRUE(ok);
    EXPECT_FALSE(session->watching()) << "a session whose transport was taken still watches the descriptor";

    const std::string_view line = "for the new owner\n";
    ASSERT_EQ(peer.write(line.data(), line.size()), static_cast<int>(line.size()));
    linger_test::pump_for(100ms);
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
    EXPECT_EQ(g_messages, 0);
    // The octets are the new owner's: read until they are all there, or a deadline a loaded host does not reach.
    std::string got;
    ASSERT_EQ(sock.set_nonblocking(true), 0);
    const auto deadline = linger_test::linger_clock::now() + 5s;
    while (got.size() < line.size() && linger_test::linger_clock::now() < deadline) {
        char      chunk[64];
        const int n = sock.read(chunk, sizeof chunk);
        if (n > 0)
            got.append(chunk, static_cast<std::size_t>(n));
        else
            linger_test::pump_for(1ms);
    }
    EXPECT_EQ(got, line);
    session.reset();
    sock.close();
    peer.close();
}

namespace extracted_timed {

int g_timeouts     = 0; ///< inactivity timeouts the session ran
int g_disconnected = 0; ///< `disconnected` events the session was handed
int g_own_ticks    = 0; ///< ticks of the timer a session keeps for something of its own

class TimedServer;

/// A session with an inactivity timer (`use<T>::timeout`) and NO `extracted` hook: nothing of its own stops that
/// timer when its transport is taken. Its timeout does what a session's does: it disconnects.
class TimedSession
    : public use<TimedSession>::tcp::client<TimedServer>
    , public use<TimedSession>::timeout {
public:
    using Protocol = qb::protocol::text::command<TimedSession>;

    explicit TimedSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::timer const &) {
        ++g_timeouts;
        this->disconnect();
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class TimedServer : public use<TimedServer>::tcp::server<TimedSession> {};

class FinalTimedServer;

/// The same session declared final, naming nothing (the eighteenth independent review): the probe cannot derive from
/// it, the name is looked up in it directly and not found, and its `use<T>::timeout` is found by its type.
class FinalTimedSession final
    : public use<FinalTimedSession>::tcp::client<FinalTimedServer>
    , public use<FinalTimedSession>::timeout {
public:
    using Protocol = qb::protocol::text::command<FinalTimedSession>;

    explicit FinalTimedSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::timer const &) {
        ++g_timeouts;
        this->disconnect();
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class FinalTimedServer : public use<FinalTimedServer>::tcp::server<FinalTimedSession> {};

/// A timer a session keeps, as a base, for something of its own: one tick, `g_own_ticks`.
struct OwnTimer : async::with_timeout<OwnTimer> {
    OwnTimer()
        : async::with_timeout<OwnTimer>(qb::duration::zero()) {}
    void
    on(async::event::timer const &) {
        ++g_own_ticks;
    }
};

class TwoTimersServer;

/// The same session with a second `with_timeout` base, its own timer: the extraction finds the inactivity one by its
/// type (the deduction of either base was ambiguous, and stopped neither), and leaves the other alone.
class TwoTimersSession
    : public use<TwoTimersSession>::tcp::client<TwoTimersServer>
    , public use<TwoTimersSession>::timeout
    , public OwnTimer {
public:
    using Protocol = qb::protocol::text::command<TwoTimersSession>;

    explicit TwoTimersSession(IOServer &server)
        : client(server) {}

    [[nodiscard]] async::with_timeout<TwoTimersSession> &
    inactivity() noexcept {
        return *this;
    }
    [[nodiscard]] OwnTimer &
    own_timer() noexcept {
        return *this;
    }

    void
    on(Protocol::message &&) {}

    void
    on(async::event::timer const &) {
        ++g_timeouts;
        this->disconnect();
    }

    void
    on(async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class TwoTimersServer : public use<TwoTimersServer>::tcp::server<TwoTimersSession> {};

/// A session of @p Server, connected, then extracted and held, its inactivity timer of 100 ms armed right before the
/// extraction (@p arm): the loop runs four of its timeouts, then @p check reads the session.
template <typename Server, typename Arm, typename Check>
void
hold_past_the_extraction(Arm &&arm, Check &&check) {
    Server server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    server.start();

    qb::io::tcp::socket peer;
    ASSERT_EQ(peer.connect_v4("127.0.0.1", port), SocketStatus::Done);
    ASSERT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 5s));
    const auto id      = server.sessions().begin()->first;
    auto       session = server.session(id); // kept alive past the extraction
    arm(*session);
    auto [sock, ok] = server.extractSession(id);
    ASSERT_TRUE(ok);
    async::run_for(400ms); // four timeouts
    check(*session);
    session.reset();
    sock.close();
    peer.close();
}

/// The inactivity timer a session has through a base, as qbm-http's server sessions have theirs: its timeout,
/// `g_timeouts`, disconnects the session, as a session's does.
template <typename _Self>
struct InactivityTimer : async::with_timeout<InactivityTimer<_Self>> {
    InactivityTimer()
        : async::with_timeout<InactivityTimer<_Self>>(qb::duration::zero()) {}
    void
    on(async::event::timer const &) {
        ++g_timeouts;
        static_cast<_Self &>(*this).disconnect();
    }
};

/// An inactivity timer whose type declares a `setTimeout()` of its own, which sets a timer the type holds, not itself
/// (the twenty-second independent review): the extraction stops the named type's `with_timeout`, and never calls that
/// `setTimeout()`.
template <typename _Self>
struct HidingTimer : async::with_timeout<HidingTimer<_Self>> {
    OwnTimer held;       ///< the timer the hiding `setTimeout()` sets
    int      hidden = 0; ///< calls of the hiding `setTimeout()`
    HidingTimer()
        : async::with_timeout<HidingTimer<_Self>>(qb::duration::zero()) {}
    void
    setTimeout(qb::duration timeout) noexcept { // hides the with_timeout's own
        ++hidden;
        held.setTimeout(timeout);
    }
    [[nodiscard]] async::with_timeout<HidingTimer<_Self>> &
    timer() noexcept {
        return *this;
    }
    void
    on(async::event::timer const &) {
        ++g_timeouts;
        static_cast<_Self &>(*this).disconnect();
    }
};

class HidingTimerServer;

/// A session that names that timer as its inactivity timer.
class HidingTimerSession
    : public use<HidingTimerSession>::tcp::client<HidingTimerServer>
    , public HidingTimer<HidingTimerSession> {
public:
    using Protocol           = qb::protocol::text::command<HidingTimerSession>;
    using inactivity_timer_t = HidingTimer<HidingTimerSession>;

    explicit HidingTimerSession(IOServer &server)
        : client(server) {}

    [[nodiscard]] HidingTimer<HidingTimerSession> &
    inactivity() noexcept {
        return *this;
    }

    void
    on(Protocol::message &&) {}

    void
    on(async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class HidingTimerServer : public use<HidingTimerServer>::tcp::server<HidingTimerSession> {};

class NamedTimerServer;

/// A session that keeps a `use<T>::timeout` of its own beside the inactivity timer it has through a base, and names the
/// inactivity one (`inactivity_timer_t`).
class NamedTimerSession
    : public use<NamedTimerSession>::tcp::client<NamedTimerServer>
    , public InactivityTimer<NamedTimerSession>
    , public use<NamedTimerSession>::timeout {
public:
    using Protocol           = qb::protocol::text::command<NamedTimerSession>;
    using inactivity_timer_t = InactivityTimer<NamedTimerSession>;

    explicit NamedTimerSession(IOServer &server)
        : client(server)
        , async::with_timeout<NamedTimerSession>(qb::duration::zero()) {}

    [[nodiscard]] InactivityTimer<NamedTimerSession> &
    inactivity() noexcept {
        return *this;
    }
    [[nodiscard]] async::with_timeout<NamedTimerSession> &
    own_timer() noexcept {
        return *this;
    }

    void
    on(Protocol::message &&) {}

    void
    on(async::event::timer const &) {
        ++g_own_ticks;
    }

    void
    on(async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class NamedTimerServer : public use<NamedTimerServer>::tcp::server<NamedTimerSession> {};

class FinalNamedTimerServer;

/// The same session declared final: the probe cannot derive from it, and the name -- a public alias -- is looked up
/// in it directly (the seventeenth independent review: no case read that branch).
class FinalNamedTimerSession final
    : public use<FinalNamedTimerSession>::tcp::client<FinalNamedTimerServer>
    , public InactivityTimer<FinalNamedTimerSession>
    , public use<FinalNamedTimerSession>::timeout {
public:
    using Protocol           = qb::protocol::text::command<FinalNamedTimerSession>;
    using inactivity_timer_t = InactivityTimer<FinalNamedTimerSession>;

    explicit FinalNamedTimerSession(IOServer &server)
        : client(server)
        , async::with_timeout<FinalNamedTimerSession>(qb::duration::zero()) {}

    [[nodiscard]] InactivityTimer<FinalNamedTimerSession> &
    inactivity() noexcept {
        return *this;
    }
    [[nodiscard]] async::with_timeout<FinalNamedTimerSession> &
    own_timer() noexcept {
        return *this;
    }

    void
    on(Protocol::message &&) {}

    void
    on(async::event::timer const &) {
        ++g_own_ticks;
    }

    void
    on(async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class FinalNamedTimerServer : public use<FinalNamedTimerServer>::tcp::server<FinalNamedTimerSession> {};

class DerivedTimedServer;

/// A session whose inactivity timer is its base's `use<Base>::timeout` -- the type the server registers derives from that
/// base, with no `with_timeout` of its own type: found by deduction.
class TimedBase
    : public use<TimedBase>::tcp::client<DerivedTimedServer>
    , public use<TimedBase>::timeout {
public:
    using Protocol = qb::protocol::text::command<TimedBase>;

    explicit TimedBase(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}

    void
    on(async::event::timer const &) {
        ++g_timeouts;
        this->disconnect();
    }

    void
    on(async::event::disconnected &&) {
        ++g_disconnected;
    }
};

class DerivedTimedSession : public TimedBase {
public:
    explicit DerivedTimedSession(DerivedTimedServer &server);
};

class DerivedTimedServer : public use<DerivedTimedServer>::tcp::server<DerivedTimedSession> {};

DerivedTimedSession::DerivedTimedSession(DerivedTimedServer &server)
    : TimedBase(server) {}

} // namespace extracted_timed

// A session somebody still holds once its transport was taken is not timed out either (Huly QB-1248's twelfth
// independent review): its inactivity timer stayed armed, and at its expiry the session ran its timeout and
// disconnected -- the server told of a session it no longer has, on a connection that goes on under its new owner.
// The timer is stopped, its duration with it: getTimeout() reads 0.
TEST_F(SessionRoundtripTest, ASessionHeldPastItsExtractionIsNotTimedOut) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<TimedServer>([](TimedSession &session) { session.setTimeout(100ms); },
                                                                  [](TimedSession &session) {
                                                                      EXPECT_EQ(session.getTimeout(), qb::duration::zero())
                                                                          << "the timer was not stopped, its duration with it";
                                                                  }));
    EXPECT_EQ(g_timeouts, 0) << "the timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
}

// The same for a final session that names nothing (the eighteenth independent review): the probe's final branch finds
// no name, and the `use<T>::timeout` base is found by its type and stopped.
TEST_F(SessionRoundtripTest, AFinalSessionHeldPastItsExtractionIsNotTimedOut) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<FinalTimedServer>([](FinalTimedSession &session) { session.setTimeout(100ms); },
                                                                       [](FinalTimedSession &session) {
                                                                           EXPECT_EQ(session.getTimeout(), qb::duration::zero())
                                                                               << "the timer was not stopped, its duration with it";
                                                                       }));
    EXPECT_EQ(g_timeouts, 0) << "the timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
}

// The same for a session with a second timer of its own as a base (the thirteenth independent review): the inactivity
// timer is the `use<T>::timeout` base, found by its type, and stopped; the other is left to run.
TEST_F(SessionRoundtripTest, ASessionWithATimerOfItsOwnHeldPastItsExtractionIsNotTimedOut) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    g_own_ticks    = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<TwoTimersServer>(
        [](TwoTimersSession &session) {
            session.inactivity().setTimeout(100ms);
            session.own_timer().setTimeout(50ms);
        },
        [](TwoTimersSession &session) {
            EXPECT_EQ(session.inactivity().getTimeout(), qb::duration::zero());
            EXPECT_EQ(session.own_timer().getTimeout(), qb::duration(50ms)) << "the extraction stopped the session's own timer";
        }));
    EXPECT_EQ(g_timeouts, 0) << "the inactivity timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
    EXPECT_EQ(g_own_ticks, 1) << "the extraction stopped a timer the session keeps for something of its own";
}

// A session that names its inactivity timer (`inactivity_timer_t`, as qbm-http's server sessions name theirs) has that
// one stopped, and a `use<T>::timeout` it keeps of its own left to tick (the fourteenth independent review: found by
// its type, the session's own timer was stopped, and the inactivity one left to time the session out).
TEST_F(SessionRoundtripTest, ASessionThatNamesItsInactivityTimerHasThatOneStoppedAtItsExtraction) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    g_own_ticks    = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<NamedTimerServer>(
        [](NamedTimerSession &session) {
            session.inactivity().setTimeout(100ms);
            session.own_timer().setTimeout(50ms);
        },
        [](NamedTimerSession &session) {
            EXPECT_EQ(session.inactivity().getTimeout(), qb::duration::zero()) << "the timer the session names was not stopped";
            EXPECT_EQ(session.own_timer().getTimeout(), qb::duration(50ms)) << "the extraction stopped the session's own timer";
        }));
    EXPECT_EQ(g_timeouts, 0) << "the inactivity timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
    EXPECT_EQ(g_own_ticks, 1) << "the extraction stopped a timer the session keeps for something of its own";
}

// The same for the session declared final (the seventeenth independent review): the probe cannot derive from it, the
// name is looked up in it directly -- a public alias, seen --, and that timer stopped.
TEST_F(SessionRoundtripTest, AFinalSessionThatNamesItsInactivityTimerHasThatOneStoppedAtItsExtraction) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    g_own_ticks    = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<FinalNamedTimerServer>(
        [](FinalNamedTimerSession &session) {
            session.inactivity().setTimeout(100ms);
            session.own_timer().setTimeout(50ms);
        },
        [](FinalNamedTimerSession &session) {
            EXPECT_EQ(session.inactivity().getTimeout(), qb::duration::zero()) << "the timer the session names was not stopped";
            EXPECT_EQ(session.own_timer().getTimeout(), qb::duration(50ms)) << "the extraction stopped the session's own timer";
        }));
    EXPECT_EQ(g_timeouts, 0) << "the inactivity timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
    EXPECT_EQ(g_own_ticks, 1) << "the extraction stopped a timer the session keeps for something of its own";
}

// A session that names its inactivity timer through a type with a `setTimeout()` of its own -- one that sets a timer
// that type holds, not itself -- has the named `with_timeout` stopped, and that `setTimeout()` is never called (the
// twenty-second independent review: no case held it; called by the extraction, it would stop the timer the type holds,
// and leave the inactivity one to time the session out).
TEST_F(SessionRoundtripTest, ASessionThatNamesATimerHidingItsSetTimeoutHasItsWithTimeoutStoppedAtItsExtraction) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    g_own_ticks    = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<HidingTimerServer>(
        [](HidingTimerSession &session) {
            session.inactivity().timer().setTimeout(100ms);
            session.inactivity().held.setTimeout(50ms);
        },
        [](HidingTimerSession &session) {
            EXPECT_EQ(session.inactivity().timer().getTimeout(), qb::duration::zero()) << "the named with_timeout was not stopped";
            EXPECT_EQ(session.inactivity().hidden, 0) << "the extraction called the named type's own setTimeout()";
            EXPECT_EQ(session.inactivity().held.getTimeout(), qb::duration(50ms)) << "the extraction stopped the timer the named type holds";
        }));
    EXPECT_EQ(g_timeouts, 0) << "the inactivity timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
    EXPECT_EQ(g_own_ticks, 1) << "the extraction stopped the timer the named type holds";
}

// The same for a session whose inactivity timer belongs to a base it derives from (`use<Base>::timeout`, the session
// type registered deriving from that base, the fourteenth independent review): found by deduction, and stopped.
TEST_F(SessionRoundtripTest, ASessionWhoseTimerIsItsBasesHeldPastItsExtractionIsNotTimedOut) {
    using namespace extracted_timed;
    g_timeouts     = 0;
    g_disconnected = 0;
    ASSERT_NO_FATAL_FAILURE(hold_past_the_extraction<DerivedTimedServer>(
        [](DerivedTimedSession &session) { static_cast<TimedBase &>(session).setTimeout(100ms); },
        [](DerivedTimedSession &session) {
            EXPECT_EQ(static_cast<TimedBase &>(session).getTimeout(), qb::duration::zero()) << "the timer was not stopped";
        }));
    EXPECT_EQ(g_timeouts, 0) << "the timeout of a session whose transport was taken ran";
    EXPECT_EQ(g_disconnected, 0) << "a session whose transport was taken was told it disconnected";
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
