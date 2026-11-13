/**
 * @file qb/tests/io/system/tcp/close-after-peer-loop.cpp
 * @brief The loop's side of a close after the peer (Huly QB-1270): what it costs a thread that the loop, not the
 *        object, holds the descriptor of a connection the object ended.
 *
 * A socket the loop is closing after its peer (`detail::close_after_peer`) is a referenced watcher and a timer of that
 * thread's loop. Three consequences are the documented contract, and are pinned here as such:
 *
 *   - a program that blocks in `async::run()` until its loop is empty returns when its last closing socket is gone: at
 *     once when the peer closes on our EOF or `linger()` is zero, `linger()` later against a peer that stays silent;
 *   - the bound runs on the loop: a thread that stops running its loop keeps the descriptor until the loop runs a
 *     pass, is cleared, or the thread ends;
 *   - a thread that ends closes what its loop was closing.
 *
 * And one property the design rests on: the loop closes a descriptor NUMBER that the system hands out again at once --
 * to the object's next connection, which starts reading on it in the same pass. The backend (epoll, io_uring, poll,
 * select, wepoll, kqueue) must hear the new socket. Run this file under `QB_EV_BACKEND` for each: the fixture asserts
 * the backend that came up.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/close_after_peer_support.h"

using namespace std::chrono_literals;

namespace close_after_peer_loop_test {

using namespace qb::io::test::close_after_peer;

/**
 * @brief Connections to `listener` until one of them gets the descriptor number `wanted` (the system hands out the
 *        lowest free number on POSIX; Windows reuses a handle value often, not always).
 * @return Whether one did: then `connected` / `accepted` are that pair. The others stay open in `spare` meanwhile.
 */
struct Reuse {
    qb::io::tcp::socket              connected;
    qb::io::tcp::socket              accepted;
    std::vector<qb::io::tcp::socket> spare;
    bool                             got = false;
};

void
take_the_number(Reuse &reuse, qb::io::tcp::listener &listener, ::socket_type wanted) {
    const auto port = listener.local_endpoint().port();
    for (int i = 0; i < 256 && !reuse.got; ++i) {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        qb::io::tcp::socket a;
        ASSERT_EQ(listener.accept(a), qb::io::SocketStatus::Done);
        if (s.native_handle() == wanted) {
            reuse.connected = std::move(s);
            reuse.accepted  = std::move(a);
            reuse.got       = true;
        } else {
            reuse.spare.push_back(std::move(s));
            reuse.spare.push_back(std::move(a));
        }
    }
}

class CloseAfterPeerLoop : public CleanLoop {};

} // namespace close_after_peer_loop_test

using namespace close_after_peer_loop_test;

// The contract of a blocking run(): it returns when the last closing socket is gone. In 3.3 it returned at once -- and
// left the half-closed descriptor in the object.
TEST_F(CloseAfterPeerLoop, ABlockingRunReturnsWhenTheLastClosingSocketIsGone) {
    {
        SCOPED_TRACE("a silent peer: the bound");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(300ms);
        client.disconnect();
        const auto start = steady::now();
        qb::io::async::run();
        const double returned_ms = ms_since(start);
        EXPECT_EQ(client.dispose_events, 1u);
        EXPECT_EQ(closing(), 0u) << "run() returned with a closing socket left";
        EXPECT_GE(returned_ms, 250.) << "run() returned before the bound although the peer neither closed nor failed";
        EXPECT_LT(returned_ms, 300. + 3000.);
    }
    {
        SCOPED_TRACE("a linger of zero: at once");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(qb::duration::zero());
        client.disconnect();
        const auto start = steady::now();
        qb::io::async::run();
        EXPECT_EQ(client.dispose_events, 1u);
        EXPECT_LT(ms_since(start), 2000.) << "a zero linger waited";
        EXPECT_EQ(closing(), 0u);
    }
    {
        SCOPED_TRACE("a peer that closes: at once");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(20s);
        client.disconnect();
        ASSERT_TRUE(pump_until_disposed(client));
        ASSERT_EQ(closing(), 1u);
        pair.peer.close();
        const auto start = steady::now();
        qb::io::async::run();
        EXPECT_LT(ms_since(start), 5000.) << "run() waited for the bound although the peer had closed";
        EXPECT_EQ(closing(), 0u);
    }
}

// The bound runs on the loop. The teardown completes in the call (disconnect_now(), the modules' shape), the object is
// destroyed, and the thread does not run its loop: the descriptor stays past the bound, and the first pass closes it.
// In 3.3 it closed with the object.
TEST_F(CloseAfterPeerLoop, AThreadThatStopsRunningItsLoopKeepsTheDescriptorUntilOnePass) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->set_linger(100ms);
    client->drop_now();
    ASSERT_EQ(client->dispose_events, 1u) << "disconnect_now() outside a dispatch completes the teardown in the call";
    client.reset();
    ASSERT_EQ(closing(), 1u);
    std::this_thread::sleep_for(400ms); // past the bound, no loop pass
    EXPECT_TRUE(descriptor_open(handle)) << "something other than the loop closed the descriptor";
    EXPECT_EQ(closing(), 1u);
    qb::io::async::run(EVRUN_NOWAIT);
    qb::io::async::run(EVRUN_NOWAIT);
    EXPECT_FALSE(descriptor_open(handle)) << "the pass that follows the bound must close it";
    EXPECT_EQ(closing(), 0u);
}

// A thread makes a connection, ends it, and returns with the close pending: its loop dies with it and closes the
// descriptor.
TEST_F(CloseAfterPeerLoop, AThreadThatEndsClosesWhatItsLoopWasClosing) {
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    std::atomic<int>           connected{0}; // 1: connected, -1: could not
    std::atomic<bool>          accepted{false};
    std::atomic<::socket_type> handle{qb::io::invalid_socket};
    std::atomic<std::size_t>   closing_at_the_end{99};
    std::thread                worker([&] {
        qb::io::async::init();
        Client client;
        if (client.transport().connect_v4("127.0.0.1", pair.port) != qb::io::SocketStatus::Done) {
            connected = -1;
            return;
        }
        handle = client.transport().native_handle();
        client.set_linger(20s);
        client.start();
        connected = 1;
        client.disconnect();
        while (client.dispose_events == 0u)
            qb::io::async::run(EVRUN_NOWAIT);
        closing_at_the_end = closing();
        // The thread ends -- and its loop closes the descriptor -- only once the peer is accepted: accept() takes the
        // lowest free number, and would take the one this loop had just closed (seen under poll and select, whose
        // loops hold no descriptor of their own below it).
        while (!accepted)
            std::this_thread::sleep_for(1ms);
    });
    while (connected == 0) // the accept below blocks: it is made only once the worker's connection exists
        std::this_thread::sleep_for(1ms);
    if (connected == 1) {
        EXPECT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    }
    accepted = true;
    worker.join();
    ASSERT_EQ(connected.load(), 1) << "the worker could not connect";
    EXPECT_EQ(closing_at_the_end.load(), 1u) << "the thread ended with no close pending: the case proves nothing";
    EXPECT_FALSE(descriptor_open(handle.load())) << "the thread's descriptor outlived its loop";
    EXPECT_EQ(read_all(pair.peer, 2000ms, false).end, "EOF");
}

// The loop closes the number and the object's next connection takes it at once, three hundred times: the watcher of the
// new connection must hear its peer on every backend.
TEST_F(CloseAfterPeerLoop, TheNumberTheLoopClosedIsHeardAgainByTheNextConnection) {
    Client                client;
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto    port     = listener.local_endpoint().port();
    int           reused   = 0;
    ::socket_type previous = qb::io::invalid_socket;
    const int     rounds   = 300;
    for (int i = 0; i < rounds; ++i) {
        qb::io::tcp::socket next;
        ASSERT_EQ(next.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done) << "round " << i;
        qb::io::tcp::socket peer;
        ASSERT_EQ(listener.accept(peer), qb::io::SocketStatus::Done);
        if (next.native_handle() == previous)
            ++reused;
        previous = next.native_handle();
        if (i == 0) {
            client.transport() = std::move(next);
            client.start();
        } else {
            client.reconnect_over(std::move(next));
        }
        ASSERT_EQ(peer.write("pong\n", 5), 5);
        ASSERT_TRUE(pump_while_not([&] { return client.messages == static_cast<std::size_t>(i + 1); }))
            << "round " << i << " on " << backend_name() << ": the connection on the number reads nothing (reused " << reused
            << " times so far)";
        client.disconnect();
        ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == static_cast<std::size_t>(i + 1); }));
        peer.close();
        // The loop closes the old number; the next round connects WITHOUT another pass in between.
        ASSERT_TRUE(pump_while_not([] { return closing() == 0u; })) << "round " << i;
    }
#if !defined(_WIN32)
    EXPECT_GT(reused, rounds / 2) << "the system did not hand the closed number out again: the case proves nothing on " << backend_name();
#endif
}

// A property of the loop the design rests on, not of the closer: a socket closed BETWEEN two passes -- what a timer
// callback, another watcher's callback or a destructor does -- while the backend still has a poll on it, and its number
// taken at once by a new connection that starts reading before the next pass.
TEST_F(CloseAfterPeerLoop, ANumberClosedBetweenTwoPassesAndReusedAtOnceIsHeard) {
    Client old_client;
    Pair   old_pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(old_client, old_pair));
    pump(20ms); // the backend's poll on the old socket is pending
    const auto            handle = old_client.transport().native_handle();
    qb::io::tcp::listener listener; // before the close: it must not be the one that takes the number
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    old_client.stop();
    old_client.transport().close();

    Reuse reuse;
    ASSERT_NO_FATAL_FAILURE(take_the_number(reuse, listener, handle));
#if !defined(_WIN32)
    ASSERT_TRUE(reuse.got) << "the system did not hand the closed number out again";
#endif
    if (reuse.got) {
        Client fresh;
        fresh.transport() = std::move(reuse.connected);
        fresh.start();
        ASSERT_EQ(reuse.accepted.write("pong\n", 5), 5);
        EXPECT_TRUE(pump_while_not([&] { return fresh.messages == 1u; }))
            << "on " << backend_name() << " the new connection's watcher never fires: its registration was lost with the old socket's";
    }
}

// The BOUND ends a close -- its timer's callback closes the socket -- and a later callback of the same pass opens the
// object's next connection, which takes the number.
TEST_F(CloseAfterPeerLoop, TheBoundClosesANumberThatALaterCallbackOfTheSamePassReuses) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(40ms);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    ASSERT_EQ(closing(), 1u) << "the peer is silent: the loop holds the socket for 40 ms";

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    Reuse       reuse;
    bool        ran                 = false;
    std::size_t closing_in_callback = 99;
    // Fires after the closing socket's timer, in the same pass: the loop is not run for 80 ms, then once.
    qb::io::async::callback(
        [&] {
            ran                 = true;
            closing_in_callback = closing();
            take_the_number(reuse, listener, handle);
            if (reuse.got)
                client.reconnect_over(std::move(reuse.connected));
        },
        60ms);
    std::this_thread::sleep_for(80ms);
    qb::io::async::run(EVRUN_NOWAIT); // both timers are due: the closing socket's (40 ms) first, then the callback (60 ms)
    ASSERT_TRUE(pump_while_not([&] { return ran; }));
    EXPECT_EQ(closing_in_callback, 0u) << "the bound had not closed the socket when the later callback of the pass ran";
#if !defined(_WIN32)
    ASSERT_TRUE(reuse.got) << "the system did not hand the closed number out again";
#endif
    if (reuse.got) {
        ASSERT_EQ(reuse.accepted.write("pong\n", 5), 5);
        EXPECT_TRUE(pump_while_not([&] { return client.messages == 1u; }))
            << "on " << backend_name() << " the object's next connection never reads: its registration was lost with the closed socket's";
    }
}
