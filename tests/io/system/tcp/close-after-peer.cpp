/**
 * @file qb/tests/io/system/tcp/close-after-peer.cpp
 * @brief How a standalone qb-io object's connection ends when the object ends it (Huly QB-1270).
 *
 * In 3.3 a standalone object -- a client: no server owns it -- that ended its own connection shut its sending side
 * down and KEPT the half-closed descriptor until it was destroyed or connected again (Huly QB-1060). Nothing read
 * that socket any more: a peer that kept writing filled the two socket buffers and then blocked for ever, with no
 * error; and the close that came at last, with the peer's bytes unread, was a reset, which on Windows destroys what
 * the peer has received and not read yet -- the object's own last bytes, an HTTP/2 GOAWAY.
 *
 * Now the descriptor leaves the object in `dispose()` for its thread's loop (`detail::close_after_peer`), which sends
 * the FIN, reads and drops what the peer still sends, and closes on the first of: the peer's EOF, a socket error,
 * `linger()` (2 s by default) or 16 MiB dropped. `on(event::disconnected&&)` and `on(event::dispose&&)` run when they
 * ran; the transport is closed for the object from `dispose()` on, so it is destroyed or connected again at once.
 * A size guard of the io, or a `linger()` of zero, closes at once. An object destroyed while still connected, an
 * acceptor and a datagram object behave as before.
 *
 * The loop's side of the contract (a blocking `run()`, a thread that stops running its loop, the number handed out
 * again) is in close-after-peer-loop.cpp; TLS in close-after-peer-tls.cpp; a node that cannot be allocated in
 * close-after-peer-alloc.cpp; server sessions in session-close-after-peer.cpp; a descriptor limit and AF_UNIX in
 * close-after-peer-posix.cpp.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/close_after_peer_support.h"

using namespace std::chrono_literals;

namespace close_after_peer_test {

using namespace qb::io::test::close_after_peer;

/// A standalone acceptor: a listening socket has no stream to end.
class Acceptor : public qb::io::use<Acceptor>::tcp::acceptor {
public:
    std::size_t disconnected_events = 0;

    void
    on(qb::io::tcp::socket &&socket) {
        socket.disconnect();
    }

    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
};

/// A standalone datagram object: no stream either.
class Datagram : public qb::io::async::udp::client<Datagram> {
public:
    std::size_t disconnected_events = 0;

    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
};

/// What a peer that writes 64 KiB chunks as fast as the socket takes them, and never reads, met within `budget`.
struct Flood {
    std::size_t accepted     = 0;
    bool        write_failed = false;
    int         error        = 0;
    double      failed_ms    = -1;
};

Flood
write_without_reading(qb::io::tcp::socket &peer, std::chrono::milliseconds budget) {
    peer.set_nonblocking(true);
    Flood                   out;
    const auto              start = steady::now();
    const std::vector<char> chunk(64 * 1024, 'w');
    while (steady::now() - start < budget) {
        qb::io::async::run(EVRUN_NOWAIT);
        const int n = peer.write(chunk.data(), chunk.size());
        if (n > 0) {
            out.accepted += static_cast<std::size_t>(n);
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (n < 0 && !qb::io::socket::not_send_error(error)) {
            out.write_failed = true;
            out.error        = error;
            out.failed_ms    = ms_since(start);
            break;
        }
        std::this_thread::sleep_for(1ms); // the socket is full: the loop must read before it takes more
    }
    return out;
}

class CloseAfterPeer : public CleanLoop {};

} // namespace close_after_peer_test

using namespace close_after_peer_test;

// The defect, as a peer meets it: after our disconnect() it goes on writing and never reads. In 3.3 it filled the two
// socket buffers (2.7 MB on Linux, 327 KB on Windows) and then blocked for ever: no write failed.
TEST_F(CloseAfterPeer, APeerThatKeepsWritingSeesItsWritesFailAndNeverBlocksForEver) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    const auto bound  = std::chrono::duration_cast<std::chrono::milliseconds>(client.linger());
    ASSERT_EQ(bound, 2000ms) << "the default bound of a close after the peer";

    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_FALSE(client.transport().is_open()) << "the transport must be closed for the object once it ended the connection";

    const Flood flood = write_without_reading(pair.peer, bound + 5000ms);
    EXPECT_TRUE(flood.write_failed) << "a peer that keeps writing must see its writes fail: it had " << flood.accepted
                                    << " octets accepted and was then left blocked";
    ASSERT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the descriptor was never closed";
    EXPECT_EQ(closing(), 0u);
}

// What cuts that peer is the byte budget, not the bound: with a bound of 20 s only the budget can end the close inside
// the window. Without it the loop read 19 GB in 2 s on Linux for one such peer.
TEST_F(CloseAfterPeer, TheByteBudgetCutsAPeerThatNeverStopsWriting) {
    const std::size_t budget = qb::io::async::detail::closing_socket_byte_budget();
    Client            client;
    Pair              pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_linger(20s);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    ASSERT_EQ(closing(), 1u) << "the peer is still open: one socket is closing after it";

    const Flood flood = write_without_reading(pair.peer, 10000ms);
    EXPECT_TRUE(flood.write_failed) << "a peer that never stops writing was read for the whole bound: " << flood.accepted
                                    << " octets accepted in 10 s";
    EXPECT_GE(flood.accepted, budget) << "the peer was cut before the budget";
    EXPECT_LE(flood.accepted, budget + (std::size_t{16} << 20)) << "far more than the budget and the socket buffers was taken";
    EXPECT_TRUE(pump_while_not([] { return closing() == 0u; }));
}

// What the budget must NOT cut: a peer that had a full pipe on its way when it reads our EOF. It writes, with no loop
// pass of ours, until the two socket buffers take nothing more, and stops; the loop then reads all of it, the peer
// reads our last bytes and a clean EOF, and the close ends when the peer closes. A budget under what the buffers hold
// (measured: 1 MiB) resets that peer.
TEST_F(CloseAfterPeer, APeerThatFillsThePipeAndStopsIsReadToItsEnd) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client.out().size() == 0u; }));
    client.set_linger(20s);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));

    pair.peer.set_nonblocking(true);
    const std::vector<char> chunk(64 * 1024, 'p');
    const std::size_t       budget  = qb::io::async::detail::closing_socket_byte_budget();
    std::size_t             stuffed = 0;
    int                     blocked = 0;
    bool                    failed  = false;
    // No loop pass: nothing reads. Half the budget at most, on a host whose socket buffers would hold more than it.
    while (blocked < 50 && stuffed < budget / 2) {
        const int n = pair.peer.write(chunk.data(), chunk.size());
        if (n > 0) {
            stuffed += static_cast<std::size_t>(n);
            blocked = 0;
        } else if (n < 0 && !qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
            failed = true;
            break;
        } else {
            ++blocked;
            std::this_thread::sleep_for(1ms);
        }
    }
    ASSERT_FALSE(failed) << "the peer's writes failed before any loop pass";
    ASSERT_GT(stuffed, 4096u) << "the pipe took nothing";
    pump(300ms); // the loop reads what the pipe held; a cut would have reset the connection by now
    EXPECT_TRUE(descriptor_open(handle)) << "the budget cut a peer that had only filled the socket buffers (" << stuffed << " octets)";
    EXPECT_EQ(closing(), 1u);
    const Read got = read_all(pair.peer, 2000ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF") << "a peer that stops when it reads EOF must end without a reset (error " << got.error << ")";
    pair.peer.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the peer closed: the descriptor must close";
    EXPECT_EQ(closing(), 0u);
}

// A STANDALONE object's close_after_deliver() ends in dispose(), like its disconnect(): no server keeps it for a linger
// on the io itself, so once its output is delivered its descriptor goes to the loop and the close takes the closer's
// byte budget. (A server SESSION's close_after_deliver() lingers on the io, bounded in time alone: the upload it
// answers early may be larger than any budget -- session-close-after-peer.cpp.) Against a peer that never stops
// writing, the object's close is cut at the budget, not at its linger() of 20 s; against a peer that had bytes on
// their way and stops when it reads our EOF, the peer reads our last bytes, then EOF -- no reset, which on Windows
// would have destroyed them -- and the close ends when the peer closes.
TEST_F(CloseAfterPeer, AStandaloneObjectsCloseAfterDeliverIsCutAtTheByteBudgetNotAtItsLinger) {
    const std::size_t budget = qb::io::async::detail::closing_socket_byte_budget();
    {
        SCOPED_TRACE("a peer that never stops writing");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(20s);
        client.publish(LAST);
        client.close_after_deliver();
        ASSERT_TRUE(pump_until_disposed(client)) << "a standalone object's close_after_deliver() did not dispose once its output was delivered";
        EXPECT_FALSE(client.transport().is_open()) << "the object kept the descriptor: it lingers on the io itself, as a session does";
        ASSERT_EQ(closing(), 1u) << "the peer is still open: one socket is closing after it";

        const Flood flood = write_without_reading(pair.peer, 10000ms);
        EXPECT_TRUE(flood.write_failed) << "a peer that never stops writing was read past the budget, towards linger(): " << flood.accepted
                                        << " octets accepted in 10 s";
        EXPECT_GE(flood.accepted, budget) << "the peer was cut before the budget";
        EXPECT_LE(flood.accepted, budget + (std::size_t{16} << 20)) << "far more than the budget and the socket buffers was taken";
        EXPECT_TRUE(pump_while_not([] { return closing() == 0u; }));
    }
    {
        SCOPED_TRACE("a peer that stops when it reads our EOF");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(20s);
        client.publish(LAST);
        client.close_after_deliver();
        ASSERT_TRUE(pump_until_disposed(client));
        // The peer has bytes on their way when it meets our close: written with no loop pass of ours, far under the
        // budget. A close with them unread would be a reset, and on Windows the peer would lose our last bytes.
        pair.peer.set_nonblocking(true);
        const std::vector<char> chunk(64 * 1024, 'p');
        std::size_t             sent = 0;
        for (int blocked = 0; blocked < 50 && sent < (std::size_t{1} << 20);) {
            const int n = pair.peer.write(chunk.data(), chunk.size());
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                blocked = 0;
            } else {
                ASSERT_TRUE(n == 0 || qb::io::socket::not_send_error(qb::io::socket::get_last_errno()))
                    << "the peer's writes failed before any loop pass";
                ++blocked;
                std::this_thread::sleep_for(1ms);
            }
        }
        ASSERT_GT(sent, 4096u) << "the pipe took nothing";
        const Read got = read_all(pair.peer, 2000ms); // pumps the loop: it reads and drops what the peer sent
        EXPECT_EQ(got.received, LAST) << "the peer did not read the object's last bytes (it had sent " << sent << " octets)";
        EXPECT_EQ(got.end, "EOF") << "the peer did not read a clean end after them";
        EXPECT_EQ(closing(), 1u) << "the peer has not closed: the loop still holds the descriptor, within linger()";
        pair.peer.close();
        EXPECT_TRUE(pump_while_not([] { return closing() == 0u; })) << "the peer closed: the close must end at once, not at linger()";
    }
}

// The common case: the peer closes when it reads our EOF. Nothing may wait for the bound.
TEST_F(CloseAfterPeer, APeerThatClosesOnOurEofEndsTheCloseAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s); // only the peer's close can end it inside the test

    client.publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client.out().size() == 0u; }));
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    const Read got = read_all(pair.peer, 2000ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF");
    EXPECT_EQ(closing(), 1u) << "the peer has not closed yet: the loop holds the descriptor";
    EXPECT_TRUE(descriptor_open(handle));
    pair.peer.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the peer closed: our descriptor must close at once";
    EXPECT_EQ(closing(), 0u);
}

// The GOAWAY case: the peer's own bytes are unread when we end, it writes again, and only then reads. A close at once,
// or after a read of what is there, loses our last bytes on Windows (a reset).
TEST_F(CloseAfterPeer, OurLastBytesReachAPeerWhoseInputWasLeftUnread) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);

    client.publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client.out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096); // no loop pass reads it: the disconnect is handled first
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    pump(300ms); // a reset, if the close sent one, has reached the peer
    EXPECT_EQ(pair.peer.write("again", 5), 5);
    pump(300ms); // and the reset its own write may have drawn
    const Read got = read_all(pair.peer, 2000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "then a clean EOF, not a reset (error " << got.error << ")";
    pair.peer.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "nothing may be left once the peer closed";
}

// The object destroyed right after its dispose, the close still under way: in 3.3 its destructor closed the half-closed
// socket with input unread, and a Windows peer lost our last bytes.
TEST_F(CloseAfterPeer, TheObjectDestroyedDuringTheCloseLosesNothing) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->set_linger(20s);

    client->publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client->out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096);
    client->disconnect();
    ASSERT_TRUE(pump_until_disposed(*client));
    client.reset(); // gone
    EXPECT_TRUE(descriptor_open(handle)) << "the descriptor must outlive the object: the loop is closing it after the peer";
    pump(300ms);
    EXPECT_EQ(pair.peer.write("again", 5), 5);
    pump(300ms);
    const Read got = read_all(pair.peer, 2000ms);
    EXPECT_EQ(got.received, LAST) << "our last bytes were lost";
    EXPECT_EQ(got.end, "EOF") << "(error " << got.error << ")";
    pair.peer.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); }));
}

// The object reconnected right after its dispose -- the shape of the HTTP/2 client, of qbm-redis and qbm-pgsql. The old
// peer still gets our last bytes and a clean EOF, the new connection works and never waits.
TEST_F(CloseAfterPeer, TheObjectReconnectedDuringTheCloseLosesNothingAndDoesNotWait) {
    Client client;
    Pair   old_pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, old_pair));
    const auto old_handle = client.transport().native_handle();
    client.set_linger(20s);

    client.publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client.out().size() == 0u; }));
    const std::vector<char> unread(4096, 'u');
    ASSERT_EQ(old_pair.peer.write(unread.data(), unread.size()), 4096);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));

    Pair new_pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(new_pair));
    qb::io::tcp::socket next;
    ASSERT_EQ(next.connect_v4("127.0.0.1", new_pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(new_pair.listener.accept(new_pair.peer), qb::io::SocketStatus::Done);
    const auto start = steady::now();
    client.reconnect_over(std::move(next));
    const double reconnect_ms = ms_since(start);
    const auto   new_handle   = client.transport().native_handle();
    ASSERT_NE(new_handle, old_handle) << "the old descriptor is still open in the loop: the new connection has another";
    client.publish(std::string_view{"next"});
    pump(300ms);
    EXPECT_EQ(old_pair.peer.write("again", 5), 5);
    pump(300ms);
    const Read old_got = read_all(old_pair.peer, 2000ms);
    const Read new_got = read_all(new_pair.peer, 200ms);
    ASSERT_EQ(new_pair.peer.write("pong\n", 5), 5);
    ASSERT_TRUE(pump_while_not([&] { return client.messages == 1u; })) << "the new connection reads nothing";
    EXPECT_EQ(old_got.received, LAST) << "the old connection's last bytes were lost to the reconnection";
    EXPECT_EQ(old_got.end, "EOF") << "(error " << old_got.error << ")";
    EXPECT_EQ(new_got.received, "next");
    EXPECT_TRUE(client.is_connected());
    EXPECT_EQ(client.dispose_events, 1u) << "the old connection's close must not dispose the new one";
    EXPECT_LT(reconnect_ms, 1000.) << "a reconnection must not wait for the old connection's close";
    old_pair.peer.close();
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(old_handle); }));
    EXPECT_TRUE(client.is_connected()) << "the old descriptor's close ended the new connection";
}

// Against a peer that neither closes nor writes, on(event::dispose&&) runs when it ran -- in the pass that tears the
// connection down, with on(event::disconnected&&) -- and the loop closes the descriptor at the bound.
TEST_F(CloseAfterPeer, TheHooksKeepTheirTimingAndTheBoundClosesTheDescriptorOfASilentPeer) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(400ms);
    client.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return client.disconnected_events == 1u; }));
    const auto disposed = steady::now();
    EXPECT_EQ(client.dispose_events, 1u) << "on(event::dispose&&) must run in the dispose() that ran on(event::disconnected&&)";
    EXPECT_TRUE(client.descriptor_open_in_dispose) << "the descriptor is the loop's now, and still open: the peer has not closed";
    EXPECT_EQ(client.closing_in_dispose, 1u);
    ASSERT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the bound never closed the descriptor";
    const double closed_ms = ms_since(disposed);
    EXPECT_GE(closed_ms, 300.) << "the descriptor closed before the bound although the peer neither closed nor failed";
    EXPECT_EQ(closing(), 0u);
    EXPECT_EQ(read_all(pair.peer, 2000ms).end, "EOF");
}

// What the two hooks see of the transport. on(event::disconnected&&) is unchanged: the transport as the connection left
// it. on(event::dispose&&) after an own-ended connection now sees what it always saw after a peer-ended one: a closed
// transport, no handle, no peer -- read the peer in on(event::disconnected&&).
TEST_F(CloseAfterPeer, TheDisposeHookSeesAClosedTransportWhoeverEndedTheConnection) {
    {
        SCOPED_TRACE("ended by this object");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.disconnect();
        ASSERT_TRUE(pump_until_disposed(client));
        EXPECT_TRUE(client.open_in_disconnected);
        EXPECT_EQ(client.peer_port_at_disconnect, pair.port);
        EXPECT_FALSE(client.transport_open_in_dispose);
        EXPECT_EQ(client.peer_port_in_dispose, 0u);
        EXPECT_FALSE(client.transport().is_open());
        EXPECT_EQ(client.transport().native_handle(), qb::io::invalid_socket);
        pair.peer.close();
        ASSERT_TRUE(pump_while_not([] { return closing() == 0u; })); // nothing of this connection is left for the next
    }
    {
        SCOPED_TRACE("ended by the peer");
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        pair.peer.close();
        ASSERT_TRUE(pump_until_disposed(client));
        EXPECT_TRUE(client.open_in_disconnected);
        EXPECT_EQ(client.peer_port_at_disconnect, pair.port);
        EXPECT_FALSE(client.transport_open_in_dispose);
        EXPECT_EQ(client.peer_port_in_dispose, 0u);
        EXPECT_FALSE(client.descriptor_open_in_dispose) << "a peer-ended connection is closed outright, as before";
        EXPECT_EQ(client.closing_in_dispose, 0u);
    }
}

// The loop goes first -- listener::clear(), a thread ending -- with a close still pending: the descriptor goes with it.
TEST_F(CloseAfterPeer, ClearingTheLoopClosesWhatItWasClosing) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    ASSERT_EQ(closing(), 1u);
    ASSERT_TRUE(descriptor_open(handle));
    qb::io::async::listener::current.clear();
    EXPECT_FALSE(descriptor_open(handle)) << "a descriptor nobody owns any more survived the loop";
    EXPECT_EQ(closing(), 0u);
    EXPECT_EQ(read_all(pair.peer, 2000ms, false).end, "EOF");
}

// The last read before the final close, and what it is for. The loop is cleared while the peer has read nothing: octets
// this side wrote are still in ITS OWN send queue, the FIN behind them, and the peer's late bytes sit unread. A close
// with input unread aborts the connection and drops that queue; a close after one last read goes on delivering. So the
// peer, when it finally reads, must get every octet the socket took, then a clean EOF.
TEST_F(CloseAfterPeer, TheLastReadBeforeTheFinalCloseKeepsWhatIsStillToBeDelivered) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_linger(20s);
    const std::string payload(64 * 1024, 'd');
    for (int i = 0; i < 4096 && client.out().size() < 4 * payload.size(); ++i) { // until the socket takes no more
        client.publish(std::string_view{payload});
        qb::io::async::run(EVRUN_NOWAIT);
        qb::io::async::run(EVRUN_NOWAIT);
    }
    pump(50ms);
    client.disconnect(); // what is still in the OBJECT is dropped with it, as before
    ASSERT_TRUE(pump_until_disposed(client));
    const std::size_t taken = client.bytes_written(); // what the socket took: all of it must arrive
    ASSERT_GE(taken, 4 * payload.size()) << "the socket took too little for anything to wait behind the peer's receive buffer";
    ASSERT_EQ(closing(), 1u);
    const std::vector<char> late(4096, 'l');
    ASSERT_EQ(pair.peer.write(late.data(), late.size()), 4096); // arrives; no loop pass reads it
    std::this_thread::sleep_for(50ms);
    qb::io::async::listener::current.clear(); // the closing socket ends here, its input unread
    std::this_thread::sleep_for(100ms);
    const Read got = read_all(pair.peer, 10000ms, false);
    EXPECT_EQ(got.received.size(), taken) << "the final close dropped what was still to be delivered";
    EXPECT_EQ(got.end, "EOF") << "the socket was closed with input unread: a reset (error " << got.error << ")";
}

// The cap: at most closing_sockets_cap() sockets on a loop; one more closes the OLDEST at once. Without it a thread
// that ends connections faster than their peers close them holds close rate x bound descriptors (measured: a limit of
// 256, the 250th connect fails with EMFILE).
TEST_F(CloseAfterPeer, TheCapClosesTheOldestFirstAndTheCountNeverExceedsIt) {
    const std::size_t     cap = qb::io::async::detail::closing_sockets_cap();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto                       port = listener.local_endpoint().port();
    std::vector<qb::io::tcp::socket> peers(cap + 2);
    std::vector<::socket_type>       handles(cap + 2);
    std::size_t                      most = 0;
    for (std::size_t i = 0; i < cap + 2; ++i) {
        auto client = std::make_unique<Client>();
        ASSERT_EQ(client->transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done) << "close " << i;
        ASSERT_EQ(listener.accept(peers[i]), qb::io::SocketStatus::Done);
        handles[i] = client->transport().native_handle();
        client->set_linger(60s); // the bound cannot interfere
        client->start();
        client->disconnect();
        ASSERT_TRUE(pump_until_disposed(*client));
        client.reset();
        most = std::max(most, closing());
        // No socket is created between the dispose and these reads: a closed number has not been handed out again.
        if (i + 1 == cap) {
            EXPECT_EQ(closing(), cap);
            EXPECT_TRUE(descriptor_open(handles[0])) << "the cap closed a socket before it was reached";
        }
        if (i >= cap) {
            const std::size_t evicted = i - cap; // the oldest still closing before this close
            EXPECT_FALSE(descriptor_open(handles[evicted])) << "close " << i << " did not close the oldest closing socket";
            EXPECT_TRUE(descriptor_open(handles[evicted + 1])) << "close " << i << " closed a socket that was not the oldest";
            EXPECT_EQ(closing(), cap);
        }
    }
    EXPECT_EQ(most, cap) << "the count went above the cap";
    // From the peers' side: the evicted connection is closed (a second write fails), another still drops what it gets.
    auto second_write = [&](std::size_t i) {
        static_cast<void>(peers[i].write("x", 1));
        pump(50ms);
        return peers[i].write("x", 1);
    };
    EXPECT_EQ(second_write(0), -1) << "the evicted connection still takes writes";
    EXPECT_EQ(second_write(cap + 1), 1) << "a connection under the cap was closed";
}

// set_linger(zero) closes IN the dispose pass, with no closing socket at any moment: read inside on(event::dispose&&)
// -- after the pump a socket that lived one pass is gone.
TEST_F(CloseAfterPeer, ALingerOfZeroClosesInTheDisposePass) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_linger(qb::duration::zero());
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "a zero linger must close in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a zero linger went through a closing socket";
    EXPECT_EQ(read_all(pair.peer, 2000ms).end, "EOF");
}

// A NEGATIVE linger is "at once" too, as set_linger() says ("zero or less"): a computed bound that went below zero
// must not be read as the internal "keep the descriptor" value, which left the half-closed descriptor in the object.
TEST_F(CloseAfterPeer, ANegativeLingerClosesInTheDisposePassLikeZero) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_linger(-1ms);
    client.disconnect();
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_FALSE(client.transport_open_in_dispose) << "a negative linger kept the transport, half-closed, in the object";
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "a negative linger must close in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u);
    EXPECT_EQ(read_all(pair.peer, 2000ms).end, "EOF");
}

// A peer that had ALREADY closed when this object ends the connection costs no closing socket: what is readable is
// read in the call, the EOF is found there, and the descriptor is closed in the dispose pass.
TEST_F(CloseAfterPeer, APeerThatHadAlreadyClosedCostsNoClosingSocket) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    pair.peer.close();
    // Its FIN has ARRIVED before this object ends the connection (a loopback may deliver it after close() returns):
    // the socket reads EOF, looked at without consuming anything and without a loop pass.
    bool       eof_arrived = false;
    const auto deadline    = steady::now() + 5s;
    while (!eof_arrived && steady::now() < deadline) {
        char      octet = 0;
        const int n     = static_cast<int>(::recv(handle, &octet, 1, MSG_PEEK));
        eof_arrived     = n == 0;
        if (!eof_arrived)
            std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(eof_arrived) << "the peer's FIN never arrived";
    client.disconnect(); // ...and this object ends the connection itself
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_NE(client.reason, 0) << "the connection was ended by the peer's EOF, not by this object: the case proves nothing";
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "the peer had closed: nothing to wait for";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a closing socket was made for a peer that had already closed";
}

// A transport that was never started is still in blocking mode (a blocking connect, then a teardown that completes in
// the call: the modules' disconnect_now()). The loop takes its descriptor without blocking on it: against a peer that
// says nothing the call returns at once, where a read of a blocking socket would wait for the peer.
TEST_F(CloseAfterPeer, ATransportNeverStartedIsTakenByTheLoopWithoutBlocking) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    // The peer stays silent for 3 s, then closes: a teardown that blocked on a read returns only then.
    std::thread late_close([&pair] {
        std::this_thread::sleep_for(3000ms);
        pair.peer.close();
    });
    const auto  start = steady::now();
    client.drop_now(); // never start()ed: the teardown completes in the call
    const double returned_ms = ms_since(start);
    const bool   disposed    = client.dispose_events == 1u;
    const bool   on_the_loop = closing() == 1u && descriptor_open(handle);
    late_close.join();
    EXPECT_TRUE(disposed);
    EXPECT_LT(returned_ms, 2000.) << "the teardown waited for the peer: the descriptor was read in blocking mode";
    EXPECT_TRUE(on_the_loop) << "the silent peer's connection was not given to the loop";
    EXPECT_TRUE(pump_while_not([&] { return !descriptor_open(handle); })) << "the peer closed: the descriptor must close";
}

// A connection the io's own message-size guard ends is closed at once: nothing was said that a reset could destroy, the
// peer is the one that sent too much, and reading it for the bound would serve the flood.
TEST_F(CloseAfterPeer, AClientItsMessageSizeGuardEndsIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_max_message_size(8);
    const std::string flood = std::string(64, 'x') + "\n" + std::string(4096, 'y');
    ASSERT_EQ(pair.peer.write(flood.data(), flood.size()), static_cast<int>(flood.size()));
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_EQ(client.reason, static_cast<int>(qb::io::async::event::disconnect_reason::message_too_large));
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "a connection the size guard ended must be closed in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a flooder was handed to the loop";
    EXPECT_NE(read_all(pair.peer, 2000ms).end, "open") << "the peer of a flood must see the connection end";
}

// The reason the buffer guards end a connection with, input left unread.
TEST_F(CloseAfterPeer, AClientEndedForABufferOverflowIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const std::string unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096);
    client.disconnect(static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_EQ(client.reason, static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "a connection a buffer guard ended must be closed in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a flooder was handed to the loop";
}

// The write-buffer guard itself: the peer does not read, the client queues past its cap.
TEST_F(CloseAfterPeer, AClientItsWriteBufferGuardEndsIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    client.set_max_write_buffer_size(64 * 1024);
    const std::string chunk(1024 * 1024, 'w');
    for (int i = 0; i < 256 && client.disconnected_events == 0u; ++i) {
        client.publish(std::string_view{chunk});
        qb::io::async::run(EVRUN_NOWAIT);
    }
    ASSERT_TRUE(pump_until_disposed(client));
    EXPECT_EQ(client.reason, static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
    EXPECT_FALSE(client.descriptor_open_in_dispose) << "a connection the write-buffer guard ended must be closed in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a peer that does not read was handed to the loop";
}

// An object destroyed while still connected -- no disconnect(), no dispose -- closes at once, as before: a destructor
// makes no closing socket.
TEST_F(CloseAfterPeer, AConnectedObjectDestroyedClosesAtOnce) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->publish(LAST);
    ASSERT_TRUE(pump_while_not([&] { return client->out().size() == 0u; }));
    client.reset();
    EXPECT_FALSE(descriptor_open(handle));
    EXPECT_EQ(closing(), 0u) << "a destructor created a closing socket";
    const Read got = read_all(pair.peer, 2000ms);
    EXPECT_EQ(got.received, LAST);
    EXPECT_EQ(got.end, "EOF");
}

// An acceptor that ends itself keeps its listening socket, and with it its port, as before: a listening socket has no
// sending side and is never handed to the loop.
TEST_F(CloseAfterPeer, AnAcceptorThatDisconnectsKeepsItsListeningSocket) {
    Acceptor acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port   = acceptor.transport().local_endpoint().port();
    const auto handle = acceptor.transport().native_handle();
    acceptor.start();
    acceptor.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return acceptor.disconnected_events == 1u; }));
    pump(10ms);
    EXPECT_TRUE(acceptor.transport().is_open()) << "the listening socket left the acceptor";
    EXPECT_TRUE(descriptor_open(handle));
    EXPECT_EQ(closing(), 0u);
    qb::io::tcp::listener second;
    EXPECT_NE(second.listen_v4(port, "127.0.0.1"), 0) << "the acceptor gave its port up at its disconnect()";
}

// A datagram object that ends itself keeps its socket, as before: there is no stream to end.
TEST_F(CloseAfterPeer, ADatagramObjectThatDisconnectsKeepsItsSocket) {
    Datagram datagram;
    ASSERT_TRUE(datagram.transport().init());
    ASSERT_EQ(datagram.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto handle = datagram.transport().native_handle();
    datagram.start();
    datagram.disconnect();
    ASSERT_TRUE(pump_while_not([&] { return datagram.disconnected_events == 1u; }));
    pump(10ms);
    EXPECT_TRUE(datagram.transport().is_open()) << "the datagram socket left the object";
    EXPECT_TRUE(descriptor_open(handle));
    EXPECT_EQ(closing(), 0u);
}

// A hundred closes in a row leave nothing behind. The census is the fixture's, taken after the loop's first use:
// counted from before it, the first case of a process read one descriptor "leaked" -- the loop's own.
TEST_F(CloseAfterPeer, AHundredClosesLeaveNoDescriptorBehind) {
    for (int i = 0; i < 100; ++i) {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.publish(LAST);
        client.disconnect();
        ASSERT_TRUE(pump_until_disposed(client)) << "close " << i;
        ASSERT_EQ(read_all(pair.peer, 2000ms).end, "EOF") << "close " << i;
        pair.peer.close();
        ASSERT_TRUE(pump_while_not([] { return closing() == 0u; })) << "close " << i;
    }
    if (descriptors_at_start >= 0) {
        EXPECT_EQ(open_descriptors(), descriptors_at_start) << "a descriptor leaked over a hundred closes";
    }
}
