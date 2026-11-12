/**
 * @file qb/tests/io/system/tcp/session-close-after-peer.cpp
 * @brief How a server session's connection ends when the session ends it, and what a disposed session that is still
 *        held does (Huly QB-1270).
 *
 * A session that ends its own connection -- `disconnect()` with everything it had queued written -- is released by
 * its server at once, as before, but its descriptor no longer closes with it: the session's destructor would close
 * a socket whose peer's bytes are unread, which TCP turns into a reset, and on Windows a reset destroys what the
 * peer has received and not read yet -- the session's last bytes. The descriptor goes to the thread's loop instead
 * (`detail::close_after_peer`): the peer reads everything, then EOF, and the loop closes the descriptor when the
 * peer closes, at the session's `linger()`, or after 16 MiB dropped.
 *
 * What still closes with the session, as before: a session that already waited for its peer through
 * `close_after_deliver()` (Huly QB-1089) -- no second wait --, one with output still queued, one its own size guard
 * ended (the peer is the one flooding), and one whose `linger()` is zero.
 *
 * `close_after_deliver()` itself is unchanged and bounded in TIME alone: it is the close that lets a peer that is
 * still sending -- an upload answered early -- read the reply. A peer that writes 64 MiB into a session lingering
 * there is read to its end and reads the whole reply, then EOF.
 *
 * And a disposed session that something still holds -- `server.session(id)` is a `shared_ptr` -- is no longer
 * dispatched: its watcher is stopped at dispose whoever ended the connection. Before, it stayed started on a
 * descriptor that was readable or writable, and the loop called the dead session on every pass until it was let go
 * -- a watcher that, after the hand-over above, would also have watched a number the loop closes and the system hands
 * out again. The same stop is in `input<>::dispose()` and `output<>::dispose()`.
 *
 * The standalone side (a client that ends its own connection) is in standalone-dispose-ends-transport.cpp; the
 * POSIX-only cases (a descriptor limit, AF_UNIX) in close-after-peer-posix.cpp.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/system/allocator/pipe.h>

#include "../../shared/close_after_peer_support.h"
#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace session_close_after_peer_test {

using namespace qb::io::test::close_after_peer;

// What the session under test saw and left: one session per case; the loop, the server and the peer share the
// test's thread.
struct State {
    int                destroyed      = 0;
    int                disconnected   = 0;
    int                reason         = 0;
    bool               lingering_seen = false;
    steady::time_point command_at{};
    double             destroyed_ms           = -1; ///< since the last command
    std::size_t        closing_at_destruction = 99;
    ::socket_type      handle                 = qb::io::invalid_socket;
    qb::uuid           id{};
    qb::duration       linger           = 2s;
    std::size_t        max_message_size = 0;
};
State *g_state = nullptr;

/// The reply of the "answer" command: a few hundred octets, then `LAST`.
std::string
reply() {
    return std::string(300, 'r') + std::string(LAST);
}

class Server;

class Session : public qb::io::use<Session>::tcp::client<Server> {
public:
    using Protocol = qb::protocol::text::command<Session>;

    explicit Session(IOServer &server)
        : client(server) {}

    ~Session() {
        ++g_state->destroyed;
        g_state->destroyed_ms           = ms_since(g_state->command_at);
        g_state->closing_at_destruction = closing();
    }

    void
    on(Protocol::message &&message) {
        g_state->command_at = steady::now();
        if (message.text == "bye") { // says its last bytes and ends the connection, everything written
            *this << LAST;
            disconnect();
        } else if (message.text == "cad") { // the QB-1089 close: deliver, then wait for the peer
            *this << LAST;
            close_after_deliver();
        } else if (message.text == "answer") { // a reply that a peer still sending must read
            *this << reply();
            close_after_deliver();
        } else if (message.text == "fill") { // output the peer does not read: the socket's buffers fill
            publish(std::string(std::size_t{8} << 20, 'f'));
        } else if (message.text == "big") { // ends with output the peer has not taken
            publish(std::string(std::size_t{8} << 20, 'b'));
            disconnect();
        }
    }

    void
    on(qb::io::async::event::disconnected &&event) {
        ++g_state->disconnected;
        g_state->reason = event.reason;
    }
};

class Server : public qb::io::use<Server>::tcp::server<Session> {
public:
    void
    on(IOSession &session) {
        session.set_linger(g_state->linger);
        if (g_state->max_message_size)
            session.set_max_message_size(g_state->max_message_size);
        g_state->handle = session.transport().native_handle();
        g_state->id     = session.id();
    }
};

/// Every case leaves nothing behind: no closing socket once the loop is cleared, no descriptor (counted on POSIX).
class LoopCensus : public ::testing::Test {
protected:
    int descriptors_before = -1;

    void
    SetUp() override {
        // listener::current builds its loop on first use (Huly QB-1246): ask for it. The backend's own descriptors
        // (epoll's on Linux, the wake's on the first run) live as long as the process: the count is taken after them.
        (void) qb::io::async::listener::current.loop();
        qb::io::async::run(EVRUN_NOWAIT);
        expect_the_backend_asked_for(); // QB_EV_BACKEND, when it pins one: a run on another backend proves nothing of it
        descriptors_before = open_descriptors();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.clear();
        EXPECT_EQ(closing(), 0u) << "listener::clear() left a closing socket";
        EXPECT_EQ(open_descriptors(), descriptors_before) << "the case left a descriptor open";
    }
};

class SessionCloseAfterPeer : public LoopCensus {
protected:
    State                   state;
    std::unique_ptr<Server> server;
    qb::io::tcp::socket     peer;

    void
    SetUp() override {
        LoopCensus::SetUp();
        g_state = &state;
    }

    void
    TearDown() override {
        peer.close();
        server.reset();
        LoopCensus::TearDown();
        g_state = nullptr;
    }

    /// The server listens, the peer connects, the session is registered.
    void
    open() {
        server = std::make_unique<Server>();
        ASSERT_EQ(server->transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        const auto port = server->transport().local_endpoint().port();
        server->start();
        ASSERT_EQ(peer.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return server->session_count() == 1u; })) << "the session was never registered";
        state.command_at = steady::now();
    }

    /// Whether the session under test is lingering through close_after_deliver().
    bool
    session_lingering() {
        for (auto &[id, session] : server->sessions())
            if (session->lingering())
                return true;
        return false;
    }
};

// A minimal owner for the session branch of input<>::dispose() and output<>::dispose(): `server().disconnected(id())`.
struct Registry {
    std::size_t released = 0;

    void
    disconnected(int) noexcept {
        ++released;
    }
};

/// A server-side session on `input<>` alone: it owns its socket and frames nothing.
class InputSession : public qb::io::async::input<InputSession> {
    qb::io::tcp::socket       _transport;
    qb::allocator::pipe<char> _in;
    Registry                 &_server;

public:
    constexpr static const bool has_server = true;

    std::size_t disconnected_events = 0;

    InputSession(Registry &owner, qb::io::tcp::socket &&accepted)
        : _transport(std::move(accepted))
        , _server(owner) {}

    Registry &
    server() noexcept {
        return _server;
    }
    [[nodiscard]] int
    id() const noexcept {
        return 1;
    }
    qb::io::tcp::socket &
    transport() noexcept {
        return _transport;
    }
    qb::allocator::pipe<char> &
    in() noexcept {
        return _in;
    }
    [[nodiscard]] std::size_t
    pendingRead() const noexcept {
        return _in.size();
    }

    int
    read() noexcept {
        constexpr std::size_t chunk = 4096;
        auto *const           into  = _in.allocate_back(chunk);
        const int             ret   = _transport.read(into, chunk);
        _in.free_back(ret > 0 ? chunk - static_cast<std::size_t>(ret) : chunk);
        return ret;
    }
    void
    flush(std::size_t size) noexcept {
        _in.free_front(size);
    }
    void
    eof() noexcept {
        if (_in.empty())
            _in.reset();
        else
            _in.reorder();
    }

    void
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
    }
};

/// A protocol that never finds a message: the input session reads, keeps, and waits.
class NoMessage : public qb::io::async::AProtocol<InputSession> {
public:
    explicit NoMessage(InputSession &io) noexcept
        : AProtocol(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return 0u;
    }
    void
    onMessage(std::size_t) noexcept final {}
    void
    reset() noexcept final {}
};

/// A server-side session on `output<>` alone: it owns its socket and writes what it is given.
class OutputSession : public qb::io::async::output<OutputSession> {
    qb::io::tcp::socket       _transport;
    qb::allocator::pipe<char> _out;
    Registry                 &_server;

public:
    constexpr static const bool has_server = true;

    std::size_t disconnected_events = 0;

    OutputSession(Registry &owner, qb::io::tcp::socket &&accepted)
        : _transport(std::move(accepted))
        , _server(owner) {}

    Registry &
    server() noexcept {
        return _server;
    }
    [[nodiscard]] int
    id() const noexcept {
        return 1;
    }
    qb::io::tcp::socket &
    transport() noexcept {
        return _transport;
    }
    qb::allocator::pipe<char> &
    out() noexcept {
        return _out;
    }
    [[nodiscard]] std::size_t
    pendingWrite() const noexcept {
        return _out.size();
    }

    int
    write() noexcept {
        const int ret = _transport.write(_out.begin(), _out.size());
        if (ret > 0)
            _out.free_front(static_cast<std::size_t>(ret));
        return ret;
    }

    void
    on(qb::io::async::event::disconnected &&) noexcept {
        ++disconnected_events;
    }
};

/// A loopback pair: `accepted` for a session to own, `peer` for the test.
void
accept_pair(qb::io::tcp::socket &accepted, qb::io::tcp::socket &peer) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    ASSERT_EQ(peer.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
}

class HeldBaseSession : public LoopCensus {};

} // namespace session_close_after_peer_test

using namespace session_close_after_peer_test;

// The session's own `disconnect()`, everything written; the peer writes again after our EOF, and only then reads.
// Closing the descriptor with the session answers that write with a reset, and on Windows the reset drops our last
// bytes before the peer reads them.
TEST_F(SessionCloseAfterPeer, ASessionThatEndsItsOwnConnectionIsReleasedAtOnceAndItsDescriptorClosesAfterThePeer) {
    state.linger = 10s; // a session that waited for its peer could not be released within the pump below
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; })) << "the session was not released at once";
    EXPECT_EQ(state.disconnected, 1);
    EXPECT_EQ(server->session_count(), 0u);
    EXPECT_EQ(state.closing_at_destruction, 1u) << "the descriptor closed with the session instead of after its peer";
    EXPECT_TRUE(descriptor_open(state.handle)) << "the descriptor closed with the session: a reset for a peer that writes again";

    pump(300ms); // a reset, had the close sent one, has reached the peer
    EXPECT_EQ(peer.write("again", 5), 5) << "the peer's write after our EOF failed";
    pump(300ms); // and the reset that write would draw from a closed socket
    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";

    peer.close();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return !descriptor_open(state.handle); })) << "the descriptor outlived its peer's close";
    EXPECT_EQ(closing(), 0u);
}

// A session that lingered through close_after_deliver() until its bound has had its wait for the peer: it closes with
// the session, it is not handed to the loop for a second one.
TEST_F(SessionCloseAfterPeer, ASessionThatLingeredToItsBoundIsNotHandedToTheLoop) {
    state.linger = 300ms;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("cad\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until(
        [&] {
            state.lingering_seen = state.lingering_seen || session_lingering();
            return state.destroyed == 1;
        },
        5000ms))
        << "the lingering session was never released";
    SCOPED_TRACE("released " + std::to_string(state.destroyed_ms) + " ms after the command, linger 300 ms");
    EXPECT_TRUE(state.lingering_seen) << "the session never lingered: the case proves nothing";
    EXPECT_GE(state.destroyed_ms, 250.) << "the linger ended before its bound";
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a session that had lingered was handed to the loop: a second wait";
    EXPECT_FALSE(descriptor_open(state.handle)) << "the descriptor outlived the linger";
    const Read got = read_all(peer, 300ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF");
}

// The same linger ended by the peer's close: nothing is left to wait for.
TEST_F(SessionCloseAfterPeer, ASessionWhoseLingerThePeerEndedIsNotHandedToTheLoop) {
    state.linger = 5s;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("cad\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return session_lingering(); })) << "the session never lingered";
    const Read got = read_all(peer, 500ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF");
    peer.close();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; })) << "the peer's close did not end the linger";
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a session whose linger the peer ended was handed to the loop";
    EXPECT_FALSE(descriptor_open(state.handle));
}

// A session that ends with output still queued: it and its descriptor go at once, as before -- a peer that does not
// read cannot hold anything. Two steps, so that it holds on every system: 8 MiB at a time that the peer does not read
// until the socket stops taking them (Linux keeps a part of the first, Windows took two sends whole), then 8 MiB more
// and the disconnect().
TEST_F(SessionCloseAfterPeer, ASessionWithOutputStillQueuedClosesWithTheSession) {
    ASSERT_NO_FATAL_FAILURE(open());
    std::size_t pending = 0;
    int         fills   = 0;
    for (; fills < 32 && pending == 0; ++fills) {
        ASSERT_EQ(peer.write("fill\n", 5), 5);
        pump(30ms);
        for (auto &[id, session] : server->sessions())
            pending = session->out().size();
    }
    ASSERT_GT(pending, 0u) << "the system took " << fills << " sends of 8 MiB without the peer reading: no output was left queued";
    ASSERT_EQ(peer.write("big\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; })) << "the session was not released";
    SCOPED_TRACE(std::to_string(pending) + " octets were queued after " + std::to_string(fills) + " send(s) of 8 MiB");
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a session with output queued was handed to the loop";
    EXPECT_FALSE(descriptor_open(state.handle));
}

// A session its own size guard ends: the peer is the one flooding, and reading it for the bound would serve the flood
// the guard just refused.
TEST_F(SessionCloseAfterPeer, ASessionItsSizeGuardEndsClosesAtOnce) {
    state.max_message_size = 8;
    ASSERT_NO_FATAL_FAILURE(open());
    const std::string flood = std::string(64, 'x') + "\n";
    ASSERT_EQ(peer.write(flood.data(), flood.size()), static_cast<int>(flood.size()));
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; })) << "the size guard never ended the session";
    EXPECT_EQ(state.reason, static_cast<int>(qb::io::async::event::disconnect_reason::message_too_large));
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a flooder's session was handed to the loop";
    EXPECT_FALSE(descriptor_open(state.handle));
}

// Somebody holds the session past its own-ended dispose. Its watcher was armed when it ended; a disposed session whose
// watcher is still started is dispatched on every pass the descriptor is ready -- and, its descriptor given to the
// loop, it would watch a number the loop closes and the system hands out again.
TEST_F(SessionCloseAfterPeer, ASessionStillHeldAfterItsDisposeIsNotDispatched) {
    ASSERT_NO_FATAL_FAILURE(open());
    auto held = server->session(state.id);
    ASSERT_TRUE(held);
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.disconnected == 1; })) << "the session never ended";
    pump(20ms);
    EXPECT_EQ(server->session_count(), 0u);
    EXPECT_EQ(state.destroyed, 0) << "the case holds the session: it must still be alive";
    const std::size_t events = events_in(200);
    EXPECT_EQ(events, 0u) << "a disposed session that is still held was dispatched " << events << " time(s) in 200 idle loop passes";
    EXPECT_FALSE(held->transport().is_open()) << "the held session still owns the descriptor it gave to the loop";

    const Read got = read_all(peer, 1000ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF");
    peer.close();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return !descriptor_open(state.handle); }))
        << "the descriptor waits for the session that is still held";
    held.reset();
    EXPECT_EQ(state.destroyed, 1);
}

// The same after a PEER-ended dispose: the peer's EOF stays readable, and a watcher left started dispatches the dead
// session on every pass until it is let go.
TEST_F(SessionCloseAfterPeer, ASessionStillHeldAfterAPeerEndedDisposeIsNotDispatched) {
    ASSERT_NO_FATAL_FAILURE(open());
    auto held = server->session(state.id);
    ASSERT_TRUE(held);
    peer.close();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.disconnected == 1; })) << "the session never read its peer's EOF";
    pump(20ms);
    EXPECT_EQ(server->session_count(), 0u);
    const std::size_t events = events_in(200);
    EXPECT_EQ(events, 0u) << "a disposed session that is still held was dispatched " << events << " time(s) in 200 idle loop passes";
    held.reset();
    EXPECT_EQ(state.destroyed, 1);
    EXPECT_FALSE(descriptor_open(state.handle));
}

// Set_linger(zero): the documented "close at once", for a session too -- no node, the descriptor closed with the
// session.
TEST_F(SessionCloseAfterPeer, ASessionWithALingerOfZeroClosesWithTheSession) {
    state.linger = qb::duration::zero();
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; })) << "the session was not released";
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a zero linger went through the loop";
    EXPECT_FALSE(descriptor_open(state.handle));
}

// close_after_deliver() lingers in TIME alone (Huly QB-1089): it is the close that lets a peer that is still sending
// -- an upload answered early -- read the reply. The session answers and lingers while its peer has not stopped
// writing; the peer then writes 64 MiB, the loop run between its writes so that the linger reads and drops them, and
// only then reads. A byte budget on that linger would end it after its budget, and the session's close with the
// peer's bytes unread would reset the peer: on Linux its next write fails, on Windows the reply it has not read is
// lost. The bound is 20 s, so that only the peer's close can end the linger here, on any host.
TEST_F(SessionCloseAfterPeer, CloseAfterDeliverLetsAPeerThatIsStillSendingReadTheReply) {
    constexpr std::size_t upload = std::size_t{64} << 20;
    state.linger                 = 20s;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("answer\n", 7), 7);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return session_lingering(); })) << "the session never lingered";

    peer.set_nonblocking(true);
    const std::vector<char> chunk(64 * 1024, 'u');
    std::size_t             accepted    = 0;
    int                     write_error = 0;
    const auto              deadline    = steady::now() + 60s;
    while (accepted < upload && steady::now() < deadline) {
        const std::size_t left = upload - accepted;
        const int         n    = peer.write(chunk.data(), left < chunk.size() ? left : chunk.size());
        if (n > 0) {
            accepted += static_cast<std::size_t>(n);
        } else {
            const int error = qb::io::socket::get_last_errno();
            if (n < 0 && !qb::io::socket::not_send_error(error)) {
                write_error = error;
                break;
            }
        }
        qb::io::async::run(EVRUN_NOWAIT); // the linger reads and drops what the peer sent
    }
    EXPECT_EQ(accepted, upload) << "the peer could not finish its upload (error " << write_error << ")";
    EXPECT_EQ(write_error, 0) << "a write of the peer failed: the session reset it";

    const Read got = read_all(peer, 5000ms);
    EXPECT_EQ(got.received, reply()) << "the peer did not read the whole reply";
    EXPECT_EQ(got.end, "EOF") << "the reply did not end with a clean EOF (error " << got.error << ")";
    EXPECT_EQ(state.disconnected, 0) << "the session ended while its peer was still open, before its bound";
    EXPECT_EQ(server->session_count(), 1u) << "the session was released while its peer was still open, before its bound";

    peer.close();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }, 10000ms))
        << "the peer's close did not end the linger: the session waits for its bound (20 s)";
}

// The same stop in input<>::dispose(): a session on input<> alone, held after its peer's EOF ended it, stays
// readable at that EOF -- a watcher left started dispatches it on every pass.
TEST_F(HeldBaseSession, AHeldInputSessionIsNotDispatchedAfterItsDispose) {
    Registry            registry;
    qb::io::tcp::socket accepted;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(accept_pair(accepted, peer));
    InputSession session(registry, std::move(accepted));
    ASSERT_NE(session.switch_protocol<NoMessage>(session), nullptr);
    session.start();
    peer.close();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return registry.released == 1u; })) << "the session never read its peer's EOF";
    EXPECT_EQ(session.disconnected_events, 1u);
    const std::size_t events = events_in(200);
    EXPECT_EQ(events, 0u) << "a disposed input session was dispatched " << events << " time(s) in 200 idle loop passes";
    EXPECT_EQ(registry.released, 1u);
}

// The same stop in output<>::dispose(): a session on output<> alone, held after its own disconnect() ended it with
// its watcher armed for writing, on a socket that stays writable.
TEST_F(HeldBaseSession, AHeldOutputSessionIsNotDispatchedAfterItsDispose) {
    Registry            registry;
    qb::io::tcp::socket accepted;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(accept_pair(accepted, peer));
    OutputSession session(registry, std::move(accepted));
    session.start(); // armed for writing
    session.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return registry.released == 1u; })) << "the session never ended";
    EXPECT_EQ(session.disconnected_events, 1u);
    const std::size_t events = events_in(200);
    EXPECT_EQ(events, 0u) << "a disposed output session was dispatched " << events << " time(s) in 200 idle loop passes";
    EXPECT_EQ(registry.released, 1u);
}
