/**
 * SCRATCH, NEVER COMMITTED -- the confirmation study's witnesses of the converged close design (train 7, QB-1270 item 3),
 * the server-session side, at the qb-io level (no HTTP): what `io<>::dispose()` does with a session's descriptor.
 * One binary, run unchanged against the base (qb 50ed9117) and against the converged prototype. `CV-S` lines.
 *
 *   S1  a session that ends its own connection, everything written: released at once, the descriptor closed after the
 *       peer -- the peer writes again after our EOF, then reads our last bytes and a clean EOF
 *   S2  a session that lingered through close_after_deliver(): no second wait when that linger ends (bound, peer's EOF)
 *   S3  a session that ends with output still queued: closed at once, as before
 *   S4  a session its size guard ends (a flooder): closed at once
 *   S5  a session someone still holds after its dispose: its watcher must not stay on the descriptor it gave away
 *   S6  set_linger(zero) on a session: closed at once (the documented opt-out; what the HTTP/2 server uses for a flood)
 */
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#if __has_include(<qb/io/async/lingering_close.h>)
#include <qb/io/async/lingering_close.h>
#define CV_HAS_CLOSER 1
#endif

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace cv_session_witness_test {

constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

using wclock = std::chrono::steady_clock;

double
ms_since(wclock::time_point since) {
    return std::chrono::duration<double, std::milli>(wclock::now() - since).count();
}

bool
descriptor_open(::socket_type s) {
    int       type = 0;
    socklen_t len  = sizeof(type);
    return ::getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char *>(&type), &len) == 0;
}

std::size_t
closing_now() {
#ifdef CV_HAS_CLOSER
    return qb::io::async::detail::closing_sockets();
#else
    return 0;
#endif
}

char const *
tree() {
#ifdef CV_HAS_CLOSER
    return "cv";
#else
    return "base";
#endif
}

void
pump(std::chrono::milliseconds how_long) {
    const auto until = wclock::now() + how_long;
    while (wclock::now() < until) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

// What the session under test saw and left (one session per case; the loop and the peer share the test's thread).
struct State {
    int                destroyed      = 0;
    int                disconnected   = 0;
    int                reason         = 0;
    int                eos            = 0;
    bool               lingering_seen = false;
    wclock::time_point command_at{};
    double             disconnected_ms        = -1; // since the command
    double             destroyed_ms           = -1;
    std::size_t        closing_at_destruction = 99;
    bool               open_at_destruction    = true;
    ::socket_type      handle                 = qb::io::invalid_socket;
    qb::uuid           id{};
    qb::duration       linger           = 2s;
    std::size_t        max_message_size = 0;
};
State *g = nullptr;

class Server;

class Session : public qb::io::use<Session>::tcp::client<Server> {
public:
    using Protocol = qb::protocol::text::command<Session>;

    explicit Session(IOServer &server)
        : client(server) {}

    ~Session() {
        ++g->destroyed;
        g->destroyed_ms           = ms_since(g->command_at);
        g->closing_at_destruction = closing_now();
        g->open_at_destruction    = descriptor_open(g->handle);
    }

    void
    on(Protocol::message &&message) {
        g->command_at = wclock::now();
        if (message.text == "bye") { // says its last bytes and ends the connection: an abort in name, everything written
            *this << LAST;
            disconnect();
        } else if (message.text == "cad") { // the QB-1089 way: deliver, then wait for the peer
            *this << LAST;
            close_after_deliver();
        } else if (message.text == "fill") { // output the peer does not read: the socket's buffers fill
            publish(std::string(std::size_t{8} << 20, 'f'));
        } else if (message.text == "big") { // ends with output the peer has not taken
            publish(std::string(std::size_t{8} << 20, 'b'));
            disconnect();
        }
    }

    void
    on(qb::io::async::event::eos &&) {
        ++g->eos;
    }

    void
    on(qb::io::async::event::disconnected &&event) {
        ++g->disconnected;
        g->reason          = event.reason;
        g->disconnected_ms = ms_since(g->command_at);
    }
};

class Server : public qb::io::use<Server>::tcp::server<Session> {
public:
    void
    on(IOSession &session) {
        session.set_linger(g->linger);
        if (g->max_message_size)
            session.set_max_message_size(g->max_message_size);
        g->handle = session.transport().native_handle();
        g->id     = session.id();
    }
};

struct Read {
    std::string received;
    char const *end   = "open";
    int         error = 0;
};

Read
read_all(qb::io::tcp::socket &peer, std::chrono::milliseconds budget) {
    peer.set_nonblocking(true);
    Read                   out;
    const auto             deadline = wclock::now() + budget;
    std::array<char, 4096> buffer{};
    do {
        qb::io::async::run(EVRUN_NOWAIT);
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            out.received.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (error == 0) {
            out.end = "EOF";
            return out;
        }
        if (!qb::io::socket::not_recv_error(error)) {
            out.end   = "RESET";
            out.error = error;
            return out;
        }
        std::this_thread::sleep_for(1ms);
    } while (wclock::now() < deadline);
    return out;
}

class CvSession : public ::testing::Test {
protected:
    State                   state;
    std::unique_ptr<Server> server;
    qb::io::tcp::socket     peer;

    void
    SetUp() override {
        qb::io::async::init();
        g = &state;
    }
    void
    TearDown() override {
        server.reset();
        qb::io::async::listener::current.clear();
        g = nullptr;
    }

    /// The server listens, the peer connects, the session is registered.
    void
    open() {
        server = std::make_unique<Server>();
        ASSERT_EQ(server->transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        const auto port = server->transport().local_endpoint().port();
        server->start();
        ASSERT_EQ(peer.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return server->session_count() == 1u; }));
        state.command_at = wclock::now();
    }
};

} // namespace cv_session_witness_test

using namespace cv_session_witness_test;

// S1 -- the server twin of W3/W4: the session ends its own connection, the peer writes again after our EOF, then reads.
// Closing the descriptor with the session (the base) answers that write with a reset: on Windows the peer loses our
// last bytes (the TCP witness's K2).
TEST_F(CvSession, S1AnOwnEndedSessionIsReleasedAtOnceAndItsDescriptorClosesAfterThePeer) {
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }));
    const std::size_t sessions_after = server->session_count();
    const std::size_t closing_after  = closing_now();
    pump(300ms); // a reset, if the close sent one, has reached the peer
    const int again = peer.write("again", 5);
    pump(300ms); // and the reset its own write may have drawn
    const Read got                    = read_all(peer, 1000ms);
    const bool open_before_peer_close = descriptor_open(state.handle);
    peer.close();
    pump(100ms);
    std::printf("CV-S S1 [%s] reason=%d; the session was destroyed %.1f ms after the command (disconnected at %.1f ms), sessions=%zu, "
                "closing right after=%zu, descriptor open at its destruction=%d; the peer wrote again=%d, read '%s' then %s (error %d); "
                "descriptor open before the peer closed=%d, after=%d closing=%zu\n",
                tree(), state.reason, state.destroyed_ms, state.disconnected_ms, sessions_after, closing_after,
                state.open_at_destruction ? 1 : 0, again, got.received.c_str(), got.end, got.error, open_before_peer_close ? 1 : 0,
                descriptor_open(state.handle) ? 1 : 0, closing_now());
    EXPECT_EQ(state.disconnected, 1);
    EXPECT_EQ(sessions_after, 0u) << "the server must release the session at once";
    EXPECT_LE(state.destroyed_ms, 100.) << "the session waited for its peer";
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_STREQ(got.end, "EOF") << "then a clean EOF, not a reset";
    EXPECT_FALSE(descriptor_open(state.handle)) << "nothing left once the peer closed";
    EXPECT_EQ(closing_now(), 0u);
#ifdef CV_HAS_CLOSER
    EXPECT_EQ(closing_after, 1u) << "the descriptor did not go to the loop";
#endif
}

// S2a -- a session that lingered through close_after_deliver() and whose BOUND ended the linger: closed then, no second wait.
TEST_F(CvSession, S2aASessionThatLingeredToItsBoundDoesNotLingerTwice) {
    state.linger = 300ms;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("cad\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] {
        for (auto &[id, session] : server->sessions())
            if (session->lingering())
                state.lingering_seen = true;
        return state.destroyed == 1;
    }));
    const Read got = read_all(peer, 300ms);
    std::printf("CV-S S2a [%s] lingering seen=%d; the session was destroyed %.0f ms after the command (linger 300 ms); at its destruction: "
                "closing=%zu, descriptor open=%d; now closing=%zu open=%d; the peer read '%s' then %s\n",
                tree(), state.lingering_seen ? 1 : 0, state.destroyed_ms, state.closing_at_destruction, state.open_at_destruction ? 1 : 0,
                closing_now(), descriptor_open(state.handle) ? 1 : 0, got.received.c_str(), got.end);
    EXPECT_TRUE(state.lingering_seen);
    EXPECT_GE(state.destroyed_ms, 290.);
    EXPECT_LE(state.destroyed_ms, 1000.);
    EXPECT_EQ(closing_now(), 0u) << "a session that had lingered was handed to the closer: a second wait";
    EXPECT_FALSE(descriptor_open(state.handle)) << "the descriptor outlived the linger";
    EXPECT_EQ(got.received, LAST);
}

// S2b -- the same linger ended by the peer's close: the dispose is not peer-ended for qb-io (the protocol is not ok),
// and there is nothing to wait for either.
TEST_F(CvSession, S2bASessionWhoseLingerThePeerEndedDoesNotLingerTwice) {
    state.linger = 5s;
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("cad\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] {
        for (auto &[id, session] : server->sessions())
            if (session->lingering())
                return true;
        return false;
    }));
    const Read got = read_all(peer, 500ms);
    peer.close();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }));
    std::printf("CV-S S2b [%s] the peer read '%s' then %s and closed; the session was destroyed %.0f ms after the command; closing=%zu "
                "descriptor open=%d\n",
                tree(), got.received.c_str(), got.end, state.destroyed_ms, closing_now(), descriptor_open(state.handle) ? 1 : 0);
    EXPECT_EQ(got.received, LAST);
    EXPECT_STREQ(got.end, "EOF");
    EXPECT_EQ(closing_now(), 0u);
    EXPECT_FALSE(descriptor_open(state.handle));
}

// S3 -- output still queued when the session ends: the session and its descriptor go at once. Two steps, so that it
// holds on every system: 8 MiB at a time that the peer does not read, until the socket stops taking them (Linux keeps
// a part of the first; Windows took two sends of 8 MiB whole), then 8 MiB more and the disconnect().
TEST_F(CvSession, S3ASessionWithOutputPendingClosesAtOnce) {
    ASSERT_NO_FATAL_FAILURE(open());
    std::size_t pending_before = 0;
    int         fills          = 0;
    for (; fills < 32 && pending_before == 0; ++fills) {
        ASSERT_EQ(peer.write("fill\n", 5), 5);
        pump(30ms);
        for (auto &[id, session] : server->sessions())
            pending_before = session->out().size();
    }
    if (pending_before == 0)
        GTEST_SKIP() << "the system took 256 MiB without the peer reading: no output can be left pending here";
    ASSERT_EQ(peer.write("big\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }));
    std::printf("CV-S S3 [%s] reason=%d; after %d send(s) of 8 MiB the socket had left %zu octets queued; the session was "
                "destroyed %.1f ms after the last command; at its destruction: closing=%zu; now closing=%zu descriptor open=%d\n",
                tree(), state.reason, fills, pending_before, state.destroyed_ms, state.closing_at_destruction, closing_now(),
                descriptor_open(state.handle) ? 1 : 0);
    EXPECT_LE(state.destroyed_ms, 200.);
    EXPECT_EQ(closing_now(), 0u) << "a session with output pending was handed to the closer";
    EXPECT_FALSE(descriptor_open(state.handle));
}

// S4 -- a session its own size guard ends: the peer is the one flooding, nothing lingers.
TEST_F(CvSession, S4ASessionEndedByItsSizeGuardClosesAtOnce) {
    state.max_message_size = 8;
    ASSERT_NO_FATAL_FAILURE(open());
    const std::string flood = std::string(64, 'x') + "\n";
    ASSERT_EQ(peer.write(flood.data(), flood.size()), static_cast<int>(flood.size()));
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }));
    std::printf("CV-S S4 [%s] reason=%d; at the session's destruction closing=%zu; now closing=%zu descriptor open=%d\n", tree(), state.reason,
                state.closing_at_destruction, closing_now(), descriptor_open(state.handle) ? 1 : 0);
    EXPECT_EQ(state.reason, -2);
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a flooder's session was handed to the closer";
    EXPECT_EQ(closing_now(), 0u);
    EXPECT_FALSE(descriptor_open(state.handle));
}

// S5 -- someone holds the session (server.session(id), a shared_ptr) past its dispose. Its watcher was armed for reading
// and writing when it ended; a disposed session whose watcher is still started is dispatched on every pass the
// descriptor is writable -- its own (the base), or the one it gave away and the loop still holds (a hand-over that does
// not stop it), and later whatever connection the number is handed to.
TEST_F(CvSession, S5ASessionStillHeldAfterItsDisposeDoesNotSpinTheLoop) {
    ASSERT_NO_FATAL_FAILURE(open());
    auto held = server->session(state.id);
    ASSERT_TRUE(held);
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.disconnected == 1; }));
    pump(20ms);
    const std::size_t sessions_after = server->session_count();
    std::size_t       events         = 0;
    for (int i = 0; i < 200; ++i) {
        qb::io::async::run(EVRUN_NOWAIT);
        events += qb::io::async::listener::current.nb_invoked_event();
    }
    const bool        transport_open = held->transport().is_open();
    const std::size_t closing_held   = closing_now();
    const Read        got            = read_all(peer, 300ms);
    peer.close();
    pump(50ms);
    const bool open_after_peer_close = descriptor_open(state.handle);
    held.reset();
    std::printf("CV-S S5 [%s] the session is held past its dispose: sessions=%zu destroyed=%d, its transport open=%d, closing=%zu; 200 idle "
                "loop passes invoked %zu event(s); the peer read '%s' then %s; after the peer closed: descriptor open=%d\n",
                tree(), sessions_after, state.destroyed, transport_open ? 1 : 0, closing_held, events, got.received.c_str(), got.end,
                open_after_peer_close ? 1 : 0);
    EXPECT_EQ(sessions_after, 0u);
    EXPECT_LE(events, 5u) << "a disposed session's watcher is dispatched on every pass";
    EXPECT_EQ(got.received, LAST);
#ifdef CV_HAS_CLOSER
    EXPECT_FALSE(transport_open) << "the held session still owns the descriptor";
    EXPECT_FALSE(open_after_peer_close) << "the descriptor waits for the held session";
#endif
}

// S5b -- an OBSERVATION, not an assertion of the converged design: the same held session after a PEER-ended dispose. No
// tree stops that watcher (the hand-over is not on this path): the peer's EOF stays readable and the disposed session
// is dispatched on every pass until whoever holds it lets go. Printed for the report; it asserts nothing about events.
TEST_F(CvSession, S5bObservationAHeldSessionAfterAPeerEndedDispose) {
    ASSERT_NO_FATAL_FAILURE(open());
    auto held = server->session(state.id);
    ASSERT_TRUE(held);
    peer.close();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.disconnected == 1; }));
    pump(20ms);
    std::size_t events = 0;
    for (int i = 0; i < 200; ++i) {
        qb::io::async::run(EVRUN_NOWAIT);
        events += qb::io::async::listener::current.nb_invoked_event();
    }
    std::printf("CV-S S5b [%s] a session held past a PEER-ended dispose (reason %d): sessions=%zu; 200 idle loop passes invoked %zu "
                "event(s)\n",
                tree(), state.reason, server->session_count(), events);
    held.reset();
    EXPECT_EQ(server->session_count(), 0u);
}

// S6 -- set_linger(zero): the documented "close at once", for a session too.
TEST_F(CvSession, S6ASessionWithALingerOfZeroClosesAtOnce) {
    state.linger = qb::duration::zero();
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_EQ(peer.write("bye\n", 4), 4);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return state.destroyed == 1; }));
    std::printf("CV-S S6 [%s] linger 0: at the session's destruction closing=%zu; now closing=%zu descriptor open=%d\n", tree(),
                state.closing_at_destruction, closing_now(), descriptor_open(state.handle) ? 1 : 0);
    EXPECT_EQ(state.closing_at_destruction, 0u) << "a zero linger went through a closing node";
    EXPECT_EQ(closing_now(), 0u);
    EXPECT_FALSE(descriptor_open(state.handle));
}
