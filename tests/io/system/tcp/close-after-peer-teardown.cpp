/**
 * @file qb/tests/io/system/tcp/close-after-peer-teardown.cpp
 * @brief Where the close after the peer (Huly QB-1270) meets the end of a thread's loop (Huly QB-1246): a connection
 *        an object ends WHILE its thread's listener is being destroyed, one ended AFTER it, one handed over on a
 *        thread whose loop was never built, and a loop cleared with as many closing sockets as it may hold.
 *
 * A qb-io object that ends its own connection gives the descriptor to its thread's loop, which closes it after the
 * peer (`detail::close_after_peer`): a read watcher and a timer registered on `listener::current`. The thread's
 * listener has rules of its own for its last moments, and the two meet in four places, each pinned here:
 *
 *   - DURING the listener's destructor. The scheduler's teardown destroys the coroutines it owns; a coroutine local
 *     that is a client -- a module client held by a parked coroutine -- ends its connection from its destructor, so
 *     a closing socket is registered on a loop about to be destroyed, AFTER `clear()` has run. `~listener` repeats
 *     `clear()` while its teardown leaves something registered: the node is destroyed, the descriptor closed, the
 *     peer reads EOF. With a single pass the descriptor stayed open for as long as the process lived.
 *   - AFTER it. An object with static storage that ends its connection in its destructor runs after the main
 *     thread's `listener::current`. To register a watcher then is a request the listener stops the process for, by
 *     name; a close is a cleanup, so it is done in the call: the FIN, what is readable read, the descriptor closed.
 *   - BEFORE it exists. `listener::current` builds its loop on the first use that needs one. A close handed over on
 *     a thread that used nothing of qb builds it; the thread's end closes the descriptor.
 *   - `listener::clear()` destroys the loop-owned nodes within a budget of sixteen times what was registered: a loop
 *     that holds `closing_sockets_cap()` closing sockets is cleared of all of them.
 *
 * The case after the loop runs in a CHILD process (a death test, re-executed): its verdict is given by a JUDGE, an
 * `atexit` handler registered before the object with static storage is built, so it runs after that object's
 * destructor; it reads the peer's end of the connection, prints its verdict and ends the child with `std::_Exit`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/close_after_peer_support.h"

using namespace std::chrono_literals;

namespace close_after_peer_teardown_test {

using namespace qb::io::test::close_after_peer;

/// Whether this thread's `listener::current` has built its loop, read on the thread's record.
bool
loop_built() {
    return qb::io::async::detail::thread_loop().raw != nullptr;
}

// ---- during the listener's destructor ----------------------------------------------------------------------------

/// What the worker thread of the first case reports.
struct Teardown {
    std::atomic<int>           connected{0}; ///< 1: connected and started, -1: could not
    std::atomic<bool>          accepted{false};
    std::atomic<::socket_type> handle{qb::io::invalid_socket};
    std::atomic<bool>          loop_built_by_start{false};
    std::atomic<int>           closing_before_the_end{-1};      ///< read by the thread before it returns
    std::atomic<int>           closing_when_the_frame_died{-1}; ///< read in the frame's destruction, after the close
    std::atomic<int>           disposed_when_the_frame_died{-1};
    std::uint16_t              port = 0;
};

/// A coroutine local that ends its connection when its frame is destroyed: a module client held by a coroutine.
struct ends_when_destroyed {
    Client    client;
    Teardown &report;

    explicit ends_when_destroyed(Teardown &r)
        : report(r) {}
    ends_when_destroyed(ends_when_destroyed const &)            = delete;
    ends_when_destroyed &operator=(ends_when_destroyed const &) = delete;
    ~ends_when_destroyed() {
        if (report.connected != 1)
            return;
        client.drop_now(); // the modules' teardown: complete in the call, the descriptor handed to the loop
        report.disposed_when_the_frame_died = static_cast<int>(client.dispose_events);
        report.closing_when_the_frame_died  = static_cast<int>(closing());
    }
};

qb::io::async::task<void>
parked_with_a_connection(Teardown &report) {
    ends_when_destroyed local{report};
    if (local.client.transport().connect_v4("127.0.0.1", report.port) != qb::io::SocketStatus::Done) {
        report.connected = -1;
        co_return;
    }
    report.handle = local.client.transport().native_handle();
    local.client.set_linger(20s);
    local.client.start();
    report.loop_built_by_start = loop_built();
    report.connected           = 1;
    co_await qb::io::async::sleep(1h); // parked: only the scheduler's teardown ends this frame
}

// ---- after it: the child and its judge ----------------------------------------------------------------------------

::socket_type g_peer       = qb::io::invalid_socket; ///< the accepted end, a raw handle: read by the judge
::socket_type g_ended      = qb::io::invalid_socket; ///< the descriptor the static object's connection had
int           g_disposed   = -1;                     ///< the object's dispose events once its destructor's call returned
bool          g_loop_gone  = false;                  ///< the thread's record when the object's destructor ran
bool          g_open_after = true;                   ///< whether the object's transport was still open after the call

/// How the raw `peer` reads the end of its stream within two seconds: 0 a clean EOF, 1 a reset, 2 still open.
int
peer_end(::socket_type peer) {
    static_cast<void>(qb::io::socket::set_nonblocking(peer, true));
    char       chunk[4096];
    const auto deadline = steady::now() + 2s;
    while (steady::now() < deadline) {
        const int n = qb::io::socket::recv(peer, chunk, static_cast<int>(sizeof(chunk)));
        if (n > 0)
            continue;
        if (n == 0)
            return 0;
        if (!qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))
            return 1;
        std::this_thread::sleep_for(1ms);
    }
    return 2;
}

[[noreturn]] void
setup_failed(char const *const what) noexcept {
    std::fprintf(stderr, "setup failed: %s\n", what);
    std::fflush(stderr);
    std::_Exit(2);
}

/// An object with static storage that ends its connection in its destructor: after the main thread's listener.
struct ends_at_exit {
    Client client;

    ends_at_exit()                                = default;
    ends_at_exit(ends_at_exit const &)            = delete;
    ends_at_exit &operator=(ends_at_exit const &) = delete;
    ~ends_at_exit() {
        g_loop_gone = qb::io::async::detail::thread_loop_gone();
        client.drop_now();
        g_disposed   = static_cast<int>(client.dispose_events);
        g_open_after = client.transport().is_open();
    }
};

/// Runs after every object with static storage: the verdict of the child is its exit code.
void
judge_closed_in_the_call() {
    const int  end    = peer_end(g_peer);
    const bool closed = !descriptor_open(g_ended);
    std::fprintf(
        stderr, "judge: after the loop %d, disposed %d, the object's transport open %d, the descriptor closed %d, the peer's end %d (0: EOF)\n",
        g_loop_gone ? 1 : 0, g_disposed, g_open_after ? 1 : 0, closed ? 1 : 0, end);
    std::fflush(stderr);
    std::_Exit(g_loop_gone && g_disposed == 1 && !g_open_after && closed && end == 0 ? 0 : 1);
}

[[noreturn]] void
a_static_client_ends_its_connection_after_the_loop() {
    std::atexit(judge_closed_in_the_call); // first: it runs after the destructor of `object`
    qb::io::tcp::listener acceptor;        // exit() runs no local destructor: it stays open for the judge
    if (acceptor.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
        setup_failed("listen");
    static ends_at_exit object; // destroyed after the main thread's `listener::current`, a thread-local
    if (object.client.transport().connect_v4("127.0.0.1", acceptor.local_endpoint().port()) != qb::io::SocketStatus::Done)
        setup_failed("connect");
    qb::io::tcp::socket peer;
    if (acceptor.accept(peer) != qb::io::SocketStatus::Done)
        setup_failed("accept");
    g_peer  = peer.release_handle();
    g_ended = object.client.transport().native_handle();
    object.client.set_linger(20s); // were the close left to a loop, it would outlive the judge's two seconds
    object.client.start();         // its watcher is registered on the main thread's loop
    qb::io::async::run(EVRUN_NOWAIT);
    if (!loop_built())
        setup_failed("the main thread's loop was not built");
    std::exit(3); // the thread-locals first (the listener), then `object`, then the judge
}

class CloseAfterPeerTeardown : public CleanLoop {};

} // namespace close_after_peer_teardown_test

using namespace close_after_peer_teardown_test;

// A parked coroutine holds a connected client; the thread returns. Its listener's destructor clears the loop, then
// its scheduler destroys the frame, whose local ends the connection: the closing socket is registered after that
// clear(), on a loop about to be destroyed. The destructor's next pass destroys it, and the descriptor with it.
TEST(CloseAfterPeerTeardownThread, AConnectionAFrameEndsDuringItsListenersTeardownIsClosedByThatTeardown) {
    Pair     pair;
    Teardown report;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    report.port = pair.port;
    std::thread worker([&report] {
        qb::io::async::coro_scheduler().spawn(parked_with_a_connection(report));
        qb::io::async::coro_scheduler().run_ready(); // the frame runs to its sleep
        report.closing_before_the_end = static_cast<int>(closing());
        // The thread ends -- and its loop closes the descriptor -- only once the peer is accepted: accept() takes
        // the lowest free number, and would take the one this loop had just closed.
        while (report.connected == 1 && !report.accepted)
            std::this_thread::sleep_for(1ms);
    });
    while (report.connected == 0)
        std::this_thread::sleep_for(1ms);
    if (report.connected == 1) {
        EXPECT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    }
    report.accepted = true;
    worker.join();
    ASSERT_EQ(report.connected.load(), 1) << "the worker could not connect";
    EXPECT_TRUE(report.loop_built_by_start.load()) << "an io object that started did not build its thread's loop";
    EXPECT_EQ(report.closing_before_the_end.load(), 0) << "the connection was alive in its frame: nothing was closing yet";
    EXPECT_EQ(report.disposed_when_the_frame_died.load(), 1) << "the frame's destruction did not complete the client's teardown";
    EXPECT_EQ(report.closing_when_the_frame_died.load(), 1)
        << "the close was not handed to the dying loop: the case is no witness of the listener's second pass";
    EXPECT_FALSE(descriptor_open(report.handle.load()))
        << "the descriptor outlived the thread's listener: its teardown left the closing socket registered";
    EXPECT_EQ(read_all(pair.peer, 2000ms, false).end, "EOF");
}

// A thread that used nothing of qb has no loop (listener::current builds it on first use). A close handed over there
// builds it, and the thread's end closes what its loop was closing.
TEST(CloseAfterPeerTeardownThread, ACloseHandedOverOnAThreadWithNoLoopBuildsItAndTheThreadsEndClosesIt) {
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    std::atomic<int>           connected{0};
    std::atomic<bool>          accepted{false};
    std::atomic<::socket_type> handle{qb::io::invalid_socket};
    std::atomic<int>           loop_before{-1}, loop_after{-1}, closing_after{-1};
    std::thread                worker([&] {
        loop_before = loop_built() ? 1 : 0;
        qb::io::tcp::socket socket;
        if (socket.connect_v4("127.0.0.1", pair.port) != qb::io::SocketStatus::Done) {
            connected = -1;
            return;
        }
        handle    = socket.native_handle();
        connected = 1;
        while (!accepted) // the peer exists before the FIN leaves, and takes no number this thread frees
            std::this_thread::sleep_for(1ms);
        qb::io::async::detail::close_after_peer(socket.release_handle(), 20s);
        closing_after = static_cast<int>(closing());
        loop_after    = loop_built() ? 1 : 0;
    });
    while (connected == 0)
        std::this_thread::sleep_for(1ms);
    if (connected == 1) {
        EXPECT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    }
    accepted = true;
    worker.join();
    ASSERT_EQ(connected.load(), 1) << "the worker could not connect";
    EXPECT_EQ(loop_before.load(), 0) << "the thread had a loop before it used anything of qb: the case proves nothing";
    EXPECT_EQ(closing_after.load(), 1) << "the peer was open and silent: the loop must hold the descriptor";
    EXPECT_EQ(loop_after.load(), 1) << "a close handed over to the loop did not build it";
    EXPECT_FALSE(descriptor_open(handle.load())) << "the thread's descriptor outlived its loop";
    EXPECT_EQ(read_all(pair.peer, 2000ms, false).end, "EOF");
}

// clear() destroys the loop-owned nodes it finds, within a budget: a loop that holds as many closing sockets as it may
// is cleared of every one, each descriptor closed and each peer at EOF.
TEST_F(CloseAfterPeerTeardown, ClearClosesEverySocketOfALoopThatHoldsAsManyAsItMay) {
    const std::size_t cap = qb::io::async::detail::closing_sockets_cap();
    Pair              pair;
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<qb::io::tcp::socket>     peers(cap);
    std::vector<::socket_type>           handles;
    for (std::size_t i = 0; i < cap; ++i) {
        auto client = std::make_unique<Client>();
        ASSERT_EQ(client->transport().connect_v4("127.0.0.1", pair.port), qb::io::SocketStatus::Done);
        ASSERT_EQ(pair.listener.accept(peers[i]), qb::io::SocketStatus::Done);
        handles.push_back(client->transport().native_handle());
        client->set_linger(20s);
        client->start();
        client->drop_now();
        ASSERT_EQ(client->dispose_events, 1u);
        clients.push_back(std::move(client));
    }
    ASSERT_EQ(closing(), cap) << "every peer is open and silent: the loop holds one closing socket per connection";
    clients.clear(); // the objects go first: nothing of them names a descriptor the loop holds
    ASSERT_EQ(closing(), cap);

    qb::io::async::listener::current.clear();

    EXPECT_EQ(closing(), 0u) << "clear() left closing sockets: its budget of owners cut them";
    std::size_t still_open = 0;
    for (const auto handle : handles)
        still_open += descriptor_open(handle) ? 1u : 0u;
    EXPECT_EQ(still_open, 0u) << "descriptors the cleared loop was closing are still open";
    std::size_t at_eof = 0;
    for (auto &peer : peers)
        at_eof += read_all(peer, 2000ms, false).end == "EOF" ? 1u : 0u;
    EXPECT_EQ(at_eof, cap) << "a peer of a cleared closing socket did not read EOF";
}

// An object with static storage ends its connection in its destructor, after the main thread's listener: the close
// is done in the call -- the FIN, then the descriptor -- and nothing is registered on the destroyed listener (a
// registration there stops the process on a named line: the child would not reach its judge).
TEST(CloseAfterPeerTeardownDeathTest, AStaticObjectThatEndsItsConnectionAfterItsThreadsLoopIsClosedInTheCall) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_client_ends_its_connection_after_the_loop(), ::testing::ExitedWithCode(0),
                "judge: after the loop 1, disposed 1, the object's transport open 0, the descriptor closed 1, the peer's end 0");
}
