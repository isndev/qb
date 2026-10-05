/**
 * @file qb/tests/io/system/tcp/teardown-throwing-handlers.cpp
 * @brief A server's teardown completes when a handler throws, or when there is no handler at all
 *        (Huly QB-256).
 *
 * `dispose()` runs the derived class's `on(event::disconnected&&)`, then steps nothing may skip: a
 * session is handed back to its server, a standalone object -- an acceptor -- has its watcher
 * stopped. A handler that threw skipped them. The listener contains what a handler throws, one WARN
 * line, so nothing crashed, which is what hid it:
 *
 *   - a session whose handler threw was never handed back: it stayed in its server's table for the
 *     life of the server, and its socket, at EOF, dispatched it on every pass while `dispose()`
 *     returned at once -- a loop spinning on nothing;
 *   - an acceptor with no `on(disconnected)` of its own reached the base's fallback, which THREW
 *     ("so the listener can propagate the fatal condition upward" -- it went nowhere): the listening
 *     watcher stayed armed on a disposed acceptor, and a client waiting in the backlog spun the loop.
 *
 * Now `dispose()` contains and logs what a hook throws and completes, and the acceptor's fallback
 * logs instead of throwing, as `tcp::server`'s does. The bases' own cases (input, output, io,
 * `disconnect_now()`) are in async-bases-framing.cpp.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace teardown_throwing_handlers_test {

class ThrowingServer;

// A session whose disconnect handler throws.
class ThrowingSession : public qb::io::use<ThrowingSession>::tcp::client<ThrowingServer> {
public:
    using Protocol = qb::protocol::text::command<ThrowingSession>;

    explicit ThrowingSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::disconnected &&) {
        throw std::runtime_error("session: on(disconnected) threw");
    }
};

class ThrowingServer : public qb::io::use<ThrowingServer>::tcp::server<ThrowingSession> {
public:
    void
    on(IOSession &) {}
};

// An acceptor with no on(event::disconnected) of its own: the base's fallback handles the event.
class BareAcceptor : public qb::io::use<BareAcceptor>::tcp::acceptor {
public:
    std::size_t accepted = 0;

    void
    on(qb::io::tcp::socket &&socket) {
        ++accepted;
        socket.disconnect();
    }
};

/// Run `passes` non-blocking loop passes; return how many events they dispatched.
std::size_t
dispatched_over(int passes) {
    std::size_t events = 0;
    for (int i = 0; i < passes; ++i)
        events += static_cast<std::size_t>(qb::io::async::run(EVRUN_NOWAIT));
    return events;
}

class TeardownThrowingHandlers : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

} // namespace teardown_throwing_handlers_test

using namespace teardown_throwing_handlers_test;

TEST_F(TeardownThrowingHandlers, ASessionWhoseHandlerThrowsIsStillHandedBackToItsServer) {
    ThrowingServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = server.transport().local_endpoint().port();
    server.start();

    {
        qb::io::tcp::socket peer;
        ASSERT_EQ(peer.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return server.session_count() == 1u; })) << "the session never registered";
        peer.disconnect(); // the session reads EOF: dispose(), whose handler throws
    }

    EXPECT_TRUE(qb::io::test::pump_until([&] { return server.session_count() == 0u; }, 1s))
        << "the session must be handed back to its server although its handler threw";
    EXPECT_EQ(dispatched_over(20), 0u) << "nothing is left to wake: no session dispatched on its EOF";
}

TEST_F(TeardownThrowingHandlers, AnAcceptorWithNoDisconnectHandlerStopsListening) {
    BareAcceptor acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = acceptor.transport().local_endpoint().port();
    acceptor.start();

    acceptor.disconnect();
    dispatched_over(10);
    EXPECT_FALSE(acceptor.is_connected());

    // Queue a client in the backlog -- the kernel completes the handshake, no accept is needed --
    // and count what the loop dispatches while it waits there.
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
    EXPECT_EQ(dispatched_over(20), 0u) << "the listening watcher must be stopped: a waiting client wakes nothing";
    EXPECT_EQ(acceptor.accepted, 0u);
    client.disconnect();
}
