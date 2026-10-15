/**
 * @file qb/tests/io/system/tcp/standalone-dispose-ends-transport.cpp
 * @brief A standalone io object's dispose() ends the transport its connection ended with (Huly QB-1060).
 *
 * A standalone object -- a client: no server owns it -- keeps its transport for as long as it lives, and
 * `dispose()` stopped its watcher and left the transport as it was. After a `disconnect()`, a protocol error
 * or the peer's EOF the connection stayed open until the object connected again or was destroyed:
 *
 *   - the peer of a `disconnect()` never saw EOF, so a server kept the connection, and the work it routed
 *     for it, alive -- qbm-http's HTTP/2 client met it as QB-1021 and closed its socket by hand;
 *   - a client whose peer hung up sat in CLOSE_WAIT for as long as the object lived.
 *
 * Now every standalone `dispose()` ends the transport once the watcher is stopped, after
 * `on(event::disconnected&&)`, which still sees it as the connection left it, and before
 * `on(event::dispose&&)`, the one hook that may free the object. When the peer's EOF or a socket error ended
 * the connection it is closed outright. When this object ended it, its sending side is shut down: the peer
 * reads EOF after everything queued, and nothing is reset -- a close would reset a peer whose bytes are still
 * unread: its next send fails, and on Windows it also drops what it has not read yet. A hook that closes the
 * transport again closes nothing else. The bases' own cases (input, output, io over a borrowed socket) are in
 * async-bases-framing.cpp.
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
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

namespace standalone_dispose_ends_transport_test {

// A standalone TCP client -- no server: the shape of every client that owns its connection.
class Client : public qb::io::use<Client>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<Client>;

    std::size_t           disconnected_events     = 0;
    std::size_t           dispose_events          = 0;
    bool                  open_in_on_disconnected = false;
    std::function<void()> dispose_hook;

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
        open_in_on_disconnected = transport().is_open();
    }

    void
    on(qb::io::async::event::dispose &&) {
        ++dispose_events;
        if (dispose_hook)
            dispose_hook();
    }
};

/// A client connected to `peer` through a loopback listener, its watcher started.
void
connect_pair(Client &client, qb::io::tcp::socket &peer) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0u);
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
    ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
    client.start();
}

/// What `peer` reads up to a clean EOF within `budget`, or nothing when the connection stays open (a peer
/// never blocks here: it reads "would block" until the budget runs out) or is reset.
std::optional<std::string>
read_to_eof(qb::io::tcp::socket &peer, std::chrono::milliseconds budget = 1000ms) {
    peer.set_nonblocking(true);
    const auto            deadline = std::chrono::steady_clock::now() + budget;
    std::string           received;
    std::array<char, 256> buffer{};
    do {
        const int n = peer.read(buffer.data(), buffer.size());
        if (n > 0) {
            received.append(buffer.data(), static_cast<std::size_t>(n));
            continue;
        }
        // tcp::socket::read() reports the peer's clean close as -1 with the error cleared.
        const int error = qb::io::socket::get_last_errno();
        if (error == 0)
            return received;
        if (!qb::io::socket::not_recv_error(error))
            return std::nullopt; // a reset, not the clean end a disposed transport sends
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
}

class StandaloneDispose : public ::testing::Test {
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

} // namespace standalone_dispose_ends_transport_test

using namespace standalone_dispose_ends_transport_test;

TEST_F(StandaloneDispose, ADisconnectEndsTheConnectionAndThePeerSeesEof) {
    Client              client;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, peer));

    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";
    EXPECT_EQ(client.disconnected_events, 1u);
    EXPECT_TRUE(client.open_in_on_disconnected) << "on(disconnected) sees the transport as the connection left it";
    EXPECT_EQ(read_to_eof(peer), std::optional<std::string>{""}) << "the peer of a disconnect() must see EOF, not a connection that stays open";
}

// The client ends the connection while bytes of its peer are still unread. Closing then would reset the peer:
// its next send fails, and on Windows it also drops what it has not read yet -- here the reply. The client
// shuts its sending side down instead.
TEST_F(StandaloneDispose, ADisconnectWithInputLeftUnreadResetsNothing) {
    Client              client;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, peer));

    client.publish(std::string_view{"reply"});
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; })) << "the reply never left";
    ASSERT_EQ(peer.write("unread", 6), 6); // no loop pass reads it: the disconnect below is handled first
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";
    EXPECT_EQ(read_to_eof(peer), std::optional<std::string>{"reply"}) << "the reply, then EOF -- not a reset that loses it";
    EXPECT_EQ(peer.write("more", 4), 4) << "the connection is half-closed, not reset: the peer may still finish sending";
}

TEST_F(StandaloneDispose, APeersEofClosesTheClientsSocket) {
    Client              client;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, peer));

    peer.close(); // the client reads EOF: dispose()
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the client never saw the EOF";
    EXPECT_FALSE(client.transport().is_open()) << "a client whose peer hung up must not keep its end open (CLOSE_WAIT)";
}

// A hook that closes the transport again -- qbm-http's HTTP/2 client does, since QB-1021 -- closes nothing
// else: the descriptor dispose() released may already belong to another socket by then.
TEST_F(StandaloneDispose, AHookThatClosesTheTransportAgainClosesNothingElse) {
    Client              client;
    qb::io::tcp::socket peer;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, peer));

    qb::io::tcp::socket fresh;
    client.dispose_hook = [&] {
        ASSERT_EQ(fresh.init(AF_INET), 0); // likely the number dispose() just released
        client.transport().disconnect();
        client.transport().close();
    };
    peer.close(); // the peer ends it: dispose() closes the client's socket before the hook runs
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; })) << "the teardown never ran";

    ASSERT_TRUE(fresh.is_open());
    int type = 0;
    EXPECT_EQ(fresh.get_optval(SOL_SOCKET, SO_TYPE, type), 0) << "the second close must not have closed another socket";
    EXPECT_EQ(type, SOCK_STREAM);
}
