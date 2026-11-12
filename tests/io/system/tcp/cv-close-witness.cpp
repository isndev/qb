/**
 * SCRATCH, NEVER COMMITTED -- the confirmation study's witnesses of the converged close design (train 7, QB-1270 item 3),
 * the standalone side. One binary, run unchanged against the base (qb 50ed9117) and against the converged prototype.
 * Every case prints a `CV-W` line with what it observed.
 *
 *   F1a  a client the io's own size guard ends (message too large) is closed at once: no closing node, no descriptor
 *   F1b  the same for the reason the buffer guards use (buffer overflow)
 *   F2   the cap: the count never exceeds closing_sockets_cap(), and one more closes the OLDEST
 *   F3   the byte budget: a peer that never stops writing is cut after closing_socket_byte_budget(), not at the bound
 *   F3b  ...and a peer that fills the two socket buffers and then stops is read to its own end, never cut
 *   F4   a failed allocation while the closing node is made: the descriptor is closed at once and nothing stays registered
 *   F5   the last read before the final close: input left unread and octets of ours still in our send queue, the loop
 *        cleared, then the peer reads -- it must get every octet the socket took, then EOF
 *   F9   set_linger(zero) closes in the dispose pass itself, with no node at any moment
 *   F7   D7: an object destroyed while still connected closes at once, as before
 *   F8   an acceptor and a datagram object that end themselves keep their socket: neither is handed over
 *   F6   the contract of D3, asserted as it is stated: a blocking run() returns within linger(); one loop pass
 *        closes what a thread that had stopped running its loop kept
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
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

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;

// ---- allocation-failure injection (F4): the calling thread's Nth allocation from now throws ----------------------
// Every form of the global allocation functions is replaced, the nothrow and the array ones included: a sanitizer's
// runtime supplies its own for whatever form is left out, and a block one of those allocated must not reach free().
namespace cv_alloc {
thread_local int  countdown = -1; // -1: off; otherwise the allocations left before the one that fails
thread_local int  seen      = 0;  // allocations seen while armed
thread_local bool fired     = false;

inline bool
fail_now() noexcept {
    if (countdown < 0)
        return false;
    ++seen;
    if (countdown-- == 0) {
        countdown = -1;
        fired     = true;
        return true;
    }
    return false;
}

inline void *
plain(std::size_t size) {
    if (fail_now())
        throw std::bad_alloc();
    if (void *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

inline void *
aligned(std::size_t size, std::align_val_t alignment) {
    if (fail_now())
        throw std::bad_alloc();
    const std::size_t a = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    void *p = _aligned_malloc(size ? size : 1, a);
#else
    void *p = nullptr;
    if (::posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, size ? size : 1) != 0)
        p = nullptr;
#endif
    if (p)
        return p;
    throw std::bad_alloc();
}

inline void
release_aligned(void *p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

template <typename Allocate>
inline void *
or_null(Allocate &&allocate) noexcept {
    try {
        return allocate();
    } catch (...) {
        return nullptr;
    }
}
} // namespace cv_alloc

// clang-format off
void *operator new(std::size_t n) { return cv_alloc::plain(n); }
void *operator new[](std::size_t n) { return cv_alloc::plain(n); }
void *operator new(std::size_t n, std::nothrow_t const &) noexcept { return cv_alloc::or_null([n] { return cv_alloc::plain(n); }); }
void *operator new[](std::size_t n, std::nothrow_t const &) noexcept { return cv_alloc::or_null([n] { return cv_alloc::plain(n); }); }
void *operator new(std::size_t n, std::align_val_t a) { return cv_alloc::aligned(n, a); }
void *operator new[](std::size_t n, std::align_val_t a) { return cv_alloc::aligned(n, a); }
void *operator new(std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return cv_alloc::or_null([n, a] { return cv_alloc::aligned(n, a); }); }
void *operator new[](std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return cv_alloc::or_null([n, a] { return cv_alloc::aligned(n, a); }); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete[](void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { cv_alloc::release_aligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { cv_alloc::release_aligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { cv_alloc::release_aligned(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { cv_alloc::release_aligned(p); }
void operator delete(void *p, std::align_val_t, std::nothrow_t const &) noexcept { cv_alloc::release_aligned(p); }
void operator delete[](void *p, std::align_val_t, std::nothrow_t const &) noexcept { cv_alloc::release_aligned(p); }
// clang-format on

namespace cv_close_witness_test {

constexpr std::string_view LAST = "LAST-BYTES-GOAWAY";

double
ms_since(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
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
    const auto until = std::chrono::steady_clock::now() + how_long;
    while (std::chrono::steady_clock::now() < until) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

class Client : public qb::io::use<Client>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<Client>;

    std::size_t disconnected_events = 0;
    std::size_t dispose_events      = 0;
    std::size_t messages            = 0;
    int         reason              = 0;
    int         arm_allocation      = -1; // F4: arm the injection between the two hooks
    // What on(event::dispose&&) saw, in the dispose pass itself: the descriptor on(event::disconnected&&) still held.
    ::socket_type handle_at_disconnected = qb::io::invalid_socket;
    std::size_t   closing_in_dispose     = 99;
    bool          open_in_dispose        = true;
    int           allocations_between    = 0;
    bool          injection_fired        = false;

    void
    on(Protocol::message &&) {
        ++messages;
    }
    void
    on(qb::io::async::event::disconnected &&event) {
        ++disconnected_events;
        reason                 = event.reason;
        handle_at_disconnected = this->transport().native_handle();
        if (arm_allocation >= 0) {
            cv_alloc::seen      = 0;
            cv_alloc::fired     = false;
            cv_alloc::countdown = arm_allocation;
        }
    }
    void
    on(qb::io::async::event::dispose &&) {
        if (arm_allocation >= 0) {
            // What was counted while armed: up to and including the one that failed (the count stops there).
            allocations_between = cv_alloc::seen;
            injection_fired     = cv_alloc::fired;
            cv_alloc::countdown = -1;
        }
        closing_in_dispose = closing_now();
        open_in_dispose    = descriptor_open(handle_at_disconnected);
        ++dispose_events;
    }
};

/// A standalone acceptor (a listening socket) and a standalone datagram object: neither is a stream to end.
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

class Datagram : public qb::io::async::udp::client<Datagram> {
public:
    std::size_t disconnected_events = 0;
    void
    on(qb::io::async::event::disconnected &&) {
        ++disconnected_events;
    }
};

struct Pair {
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   peer;
    std::uint16_t         port = 0;
};

void
listen_on(Pair &pair) {
    ASSERT_EQ(pair.listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    pair.port = pair.listener.local_endpoint().port();
    ASSERT_NE(pair.port, 0u);
}

void
connect_pair(Client &client, Pair &pair) {
    ASSERT_NO_FATAL_FAILURE(listen_on(pair));
    ASSERT_EQ(client.transport().connect_v4("127.0.0.1", pair.port), qb::io::SocketStatus::Done);
    ASSERT_EQ(pair.listener.accept(pair.peer), qb::io::SocketStatus::Done);
    client.start();
}

struct Read {
    std::string received;
    char const *end   = "open";
    int         error = 0;
};

Read
read_all(qb::io::tcp::socket &peer, std::chrono::milliseconds budget, bool pump_loop) {
    peer.set_nonblocking(true);
    Read                   out;
    const auto             deadline = std::chrono::steady_clock::now() + budget;
    std::array<char, 4096> buffer{};
    do {
        if (pump_loop)
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
    } while (std::chrono::steady_clock::now() < deadline);
    return out;
}

class CvClose : public ::testing::Test {
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

} // namespace cv_close_witness_test

using namespace cv_close_witness_test;

// F1a -- the message-size guard: the peer sends a line longer than the client accepts.
TEST_F(CvClose, F1aAClientEndedByItsMessageSizeGuardIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_max_message_size(8);
    const std::string flood = std::string(64, 'x') + "\n" + std::string(4096, 'y');
    ASSERT_EQ(pair.peer.write(flood.data(), flood.size()), static_cast<int>(flood.size()));
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    ASSERT_EQ(client.handle_at_disconnected, handle);
    const bool        open_after    = client.open_in_dispose; // read inside on(event::dispose&&): the dispose pass itself
    const std::size_t closing_after = client.closing_in_dispose;
    pump(50ms);
    const int w1 = pair.peer.write("more", 4);
    pump(50ms);
    const int  w2  = pair.peer.write("more", 4);
    const Read got = read_all(pair.peer, 300ms, true);
    std::printf("CV-W F1a [%s] reason=%d; right after the dispose: descriptor open=%d closing=%zu transport open=%d; the peer's writes "
                "afterwards: %d, %d; it reads %s (error %d)\n",
                tree(), client.reason, open_after ? 1 : 0, closing_after, client.transport().is_open() ? 1 : 0, w1, w2, got.end, got.error);
    EXPECT_EQ(client.reason, -2);
    EXPECT_FALSE(open_after) << "a connection the size guard ended must be closed in the dispose pass";
    EXPECT_EQ(closing_after, 0u) << "a flooder was handed to the closer";
    EXPECT_STRNE(got.end, "open") << "the peer of a flood must see the connection end";
}

// F1b -- the reason the buffer guards end a connection with (the read-buffer cap, the write-buffer cap): `buffer_overflow`.
// The caps themselves are not reachable from outside a standalone client (its transport is a private base), so the
// witness ends the connection with the guards' own reason, input left unread.
TEST_F(CvClose, F1bAClientEndedForABufferOverflowIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto        handle = client.transport().native_handle();
    const std::string unread(4096, 'u');
    ASSERT_EQ(pair.peer.write(unread.data(), unread.size()), 4096);
    client.disconnect(qb::io::async::event::disconnect_reason::buffer_overflow);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    ASSERT_EQ(client.handle_at_disconnected, handle);
    const bool        open_after    = client.open_in_dispose; // read inside on(event::dispose&&): the dispose pass itself
    const std::size_t closing_after = client.closing_in_dispose;
    std::printf("CV-W F1b [%s] reason=%d; right after the dispose: descriptor open=%d closing=%zu\n", tree(), client.reason, open_after ? 1 : 0,
                closing_after);
    EXPECT_EQ(client.reason, -3);
    EXPECT_FALSE(open_after) << "a connection a buffer guard ended must be closed in the dispose pass";
    EXPECT_EQ(closing_after, 0u) << "a flooder was handed to the closer";
}

// F1c -- the write-buffer guard itself: the peer does not read, the client queues past its cap (the cap is settable
// from outside since qb 0d91a634, QB-86, which both trees carry).
TEST_F(CvClose, F1cAClientEndedByItsWriteBufferGuardIsClosedAtOnce) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_max_write_buffer_size(64 * 1024);
    const std::string chunk(1024 * 1024, 'w');
    for (int i = 0; i < 256 && client.disconnected_events == 0u; ++i) {
        client.publish(std::string_view{chunk});
        qb::io::async::run(EVRUN_NOWAIT);
    }
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    ASSERT_EQ(client.handle_at_disconnected, handle);
    const bool        open_after    = client.open_in_dispose; // read inside on(event::dispose&&): the dispose pass itself
    const std::size_t closing_after = client.closing_in_dispose;
    std::printf("CV-W F1c [%s] reason=%d; right after the dispose: descriptor open=%d closing=%zu\n", tree(), client.reason, open_after ? 1 : 0,
                closing_after);
    EXPECT_EQ(client.reason, -3);
    EXPECT_FALSE(open_after) << "a connection the write-buffer guard ended must be closed in the dispose pass";
    EXPECT_EQ(closing_after, 0u) << "a peer that does not read was handed to the closer";
}

// F9 -- set_linger(zero) closes IN the dispose pass, with no node at any moment: read inside on(event::dispose&&). (The
// dossier's W9 reads after the pump, where a node that lived one loop pass has already gone.)
TEST_F(CvClose, F9ALingerOfZeroClosesInTheDisposePass) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(qb::duration::zero());
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.dispose_events == 1u; }));
    ASSERT_EQ(client.handle_at_disconnected, handle);
    const Read got = read_all(pair.peer, 300ms, true);
    std::printf("CV-W F9 [%s] linger 0: inside on(event::dispose&&) descriptor open=%d closing=%zu; the peer then reads %s\n", tree(),
                client.open_in_dispose ? 1 : 0, client.closing_in_dispose, got.end);
    EXPECT_FALSE(client.open_in_dispose) << "a zero linger must close in the dispose pass";
    EXPECT_EQ(client.closing_in_dispose, 0u) << "a zero linger went through a closing node";
    EXPECT_STREQ(got.end, "EOF");
}

// F7 -- D7: an object destroyed while still connected (no disconnect(), no dispose) closes at once, as before: no node.
TEST_F(CvClose, F7AConnectedObjectDestroyedClosesAtOnce) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client->out().size() == 0u; }));
    client.reset();
    const bool        open_after    = descriptor_open(handle);
    const std::size_t closing_after = closing_now();
    const Read        got           = read_all(pair.peer, 300ms, true);
    std::printf("CV-W F7 [%s] destroyed while connected: descriptor open=%d closing=%zu; the peer read '%s' then %s\n", tree(),
                open_after ? 1 : 0, closing_after, got.received.c_str(), got.end);
    EXPECT_FALSE(open_after);
    EXPECT_EQ(closing_after, 0u) << "a destructor created a closing node";
    EXPECT_EQ(got.received, LAST);
    EXPECT_STREQ(got.end, "EOF");
}

// F8a -- an acceptor that ends itself keeps its listening socket, and with it its port, as before: never handed over.
TEST_F(CvClose, F8aAnAcceptorThatDisconnectsKeepsItsListeningSocket) {
    Acceptor acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port   = acceptor.transport().local_endpoint().port();
    const auto handle = acceptor.transport().native_handle();
    acceptor.start();
    acceptor.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return acceptor.disconnected_events == 1u; }));
    pump(10ms);
    qb::io::tcp::listener second;
    const int             second_bind = second.listen_v4(port, "127.0.0.1");
    std::printf("CV-W F8a [%s] an acceptor after its own disconnect(): transport open=%d descriptor open=%d closing=%zu; a second "
                "listener on its port: %d (0 would mean the port was given up)\n",
                tree(), acceptor.transport().is_open() ? 1 : 0, descriptor_open(handle) ? 1 : 0, closing_now(), second_bind);
    EXPECT_TRUE(acceptor.transport().is_open()) << "the listening socket left the acceptor";
    EXPECT_TRUE(descriptor_open(handle));
    EXPECT_EQ(closing_now(), 0u);
    EXPECT_NE(second_bind, 0) << "the acceptor gave its port up at its disconnect()";
}

// F8b -- a datagram object that ends itself keeps its socket, as before: there is no stream to end.
TEST_F(CvClose, F8bADatagramObjectThatDisconnectsKeepsItsSocket) {
    Datagram datagram;
    ASSERT_TRUE(datagram.transport().init());
    ASSERT_EQ(datagram.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto handle = datagram.transport().native_handle();
    datagram.start();
    datagram.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return datagram.disconnected_events == 1u; }));
    pump(10ms);
    std::printf("CV-W F8b [%s] a datagram object after its own disconnect(): transport open=%d descriptor open=%d closing=%zu\n", tree(),
                datagram.transport().is_open() ? 1 : 0, descriptor_open(handle) ? 1 : 0, closing_now());
    EXPECT_TRUE(datagram.transport().is_open()) << "the datagram socket left the object";
    EXPECT_TRUE(descriptor_open(handle));
    EXPECT_EQ(closing_now(), 0u);
}

#ifdef CV_HAS_CLOSER
// F2 -- the cap. cap + 2 closes against peers that stay open and silent, a bound that cannot interfere.
TEST_F(CvClose, F2TheCapClosesTheOldestFirstAndTheCountNeverExceedsIt) {
    const std::size_t     cap = qb::io::async::detail::closing_sockets_cap();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto                       port = listener.local_endpoint().port();
    std::vector<qb::io::tcp::socket> peers(cap + 2);
    std::vector<::socket_type>       handles(cap + 2);
    std::size_t                      max_closing = 0;
    std::string                      order; // which of the first three descriptors each extra close found closed
    for (std::size_t i = 0; i < cap + 2; ++i) {
        auto client = std::make_unique<Client>();
        ASSERT_EQ(client->transport().connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done) << "close " << i;
        ASSERT_EQ(listener.accept(peers[i]), qb::io::SocketStatus::Done);
        handles[i] = client->transport().native_handle();
        client->set_linger(60s);
        client->start();
        client->disconnect();
        while (client->disconnected_events == 0u)
            qb::io::async::run(EVRUN_NOWAIT);
        client.reset();
        max_closing = std::max(max_closing, closing_now());
        // No socket is created between the dispose and these reads: a closed number has not been handed out again.
        if (i + 1 == cap) {
            EXPECT_EQ(closing_now(), cap);
            EXPECT_TRUE(descriptor_open(handles[0])) << "the cap closed a socket before it was reached";
        }
        if (i >= cap) {
            const std::size_t evicted = i - cap; // the oldest still closing before this close
            order += descriptor_open(handles[evicted]) ? "open " : "closed ";
            order += descriptor_open(handles[evicted + 1]) ? "open; " : "closed; ";
            EXPECT_FALSE(descriptor_open(handles[evicted])) << "close " << i << " did not close the oldest closing socket";
            EXPECT_TRUE(descriptor_open(handles[evicted + 1])) << "close " << i << " closed a socket that was not the oldest";
            EXPECT_EQ(closing_now(), cap);
        }
    }
    // From the peers' side: the evicted connections are closed (a second write fails), the others still drop what they get.
    auto second_write = [&](std::size_t i) {
        static_cast<void>(peers[i].write("x", 1));
        pump(30ms);
        return peers[i].write("x", 1);
    };
    const int oldest = second_write(0), kept = second_write(cap + 1);
    std::printf("CV-W F2 [%s] cap=%zu: %zu closes, closing max=%zu now=%zu; (oldest, next) at each close past the cap: %s; a peer's second "
                "write: evicted=%d kept=%d\n",
                tree(), cap, cap + 2, max_closing, closing_now(), order.c_str(), oldest, kept);
    EXPECT_EQ(max_closing, cap) << "the count went above the cap";
    EXPECT_EQ(oldest, -1) << "the evicted connection still takes writes";
    EXPECT_EQ(kept, 1) << "a connection under the cap was closed";
}

// F3 -- the byte budget. A peer that writes as fast as it can and never reads; a bound of 20 s so that only the budget
// can cut it within the window.
TEST_F(CvClose, F3TheByteBudgetCutsAPeerThatNeverStopsWriting) {
    const std::size_t budget = qb::io::async::detail::closing_socket_byte_budget();
    Client            client;
    Pair              pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.set_linger(20s);
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    const std::size_t closing_before = closing_now();

    pair.peer.set_nonblocking(true);
    const auto              start = std::chrono::steady_clock::now();
    const std::vector<char> chunk(64 * 1024, 'w');
    std::size_t             accepted  = 0;
    double                  failed_at = -1, closed_at = -1;
    int                     fail_errno = 0;
    while (ms_since(start) < 6000.) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (closed_at < 0 && !descriptor_open(handle))
            closed_at = ms_since(start);
        const int n = pair.peer.write(chunk.data(), chunk.size());
        if (n > 0) {
            accepted += static_cast<std::size_t>(n);
            continue;
        }
        const int error = qb::io::socket::get_last_errno();
        if (n < 0 && !qb::io::socket::not_send_error(error)) {
            failed_at  = ms_since(start);
            fail_errno = error;
            break;
        }
    }
    std::printf("CV-W F3 [%s] budget=%zu, linger 20 s: closing before=%zu; the peer had %zu octets accepted when its write failed at "
                "%.1f ms (errno %d; -1: not within 6 s); descriptor closed at %.1f ms; closing=%zu\n",
                tree(), budget, closing_before, accepted, failed_at, fail_errno, closed_at, closing_now());
    EXPECT_EQ(closing_before, 1u);
    EXPECT_GE(failed_at, 0.) << "a peer that never stops writing was read for the whole bound: no byte budget";
    EXPECT_GE(accepted, budget) << "the peer was cut before the budget";
    EXPECT_LE(accepted, budget + (std::size_t{16} << 20)) << "far more than the budget and the socket buffers was taken";
    EXPECT_EQ(closing_now(), 0u);
}

// F3b -- what the budget must NOT cut: a peer that had a full pipe on its way when it reads our EOF. It writes, with no
// loop pass of ours, until the two socket buffers take nothing more, and stops; the closer then reads all of it, the
// peer reads our last bytes and a clean EOF, and the close ends when the peer closes -- no reset anywhere.
TEST_F(CvClose, F3bAPeerThatFillsThePipeAndStopsIsReadToItsEnd) {
    const std::size_t budget = qb::io::async::detail::closing_socket_byte_budget();
    Client            client;
    Pair              pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const auto handle = client.transport().native_handle();
    client.publish(LAST);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.out().size() == 0u; }));
    client.set_linger(20s);
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    pair.peer.set_nonblocking(true);
    const std::vector<char> chunk(64 * 1024, 'p');
    std::size_t             stuffed = 0;
    int                     blocked = 0, failed = 0;
    while (blocked < 50 && stuffed < (std::size_t{256} << 20)) { // no loop pass: nothing reads
        const int n = pair.peer.write(chunk.data(), chunk.size());
        if (n > 0) {
            stuffed += static_cast<std::size_t>(n);
            blocked = 0;
        } else if (n < 0 && !qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {
            failed = 1;
            break;
        } else {
            ++blocked;
            std::this_thread::sleep_for(1ms);
        }
    }
    pump(300ms); // the closer reads what the pipe held; a cut would have reset the connection by now
    const bool        open_after_the_drain = descriptor_open(handle);
    const std::size_t closing_then         = closing_now();
    const int         one_more             = pair.peer.write("x", 1);
    const Read        got                  = read_all(pair.peer, 500ms, true);
    pair.peer.close();
    pump(50ms);
    std::printf("CV-W F3b [%s] budget=%zu: the peer filled the pipe with %zu octets and stopped (write failed=%d); after the closer "
                "read them: descriptor open=%d closing=%zu, one more octet written=%d; the peer read '%s' then %s (error %d); after its "
                "close: descriptor open=%d closing=%zu\n",
                tree(), budget, stuffed, failed, open_after_the_drain ? 1 : 0, closing_then, one_more, got.received.c_str(), got.end, got.error,
                descriptor_open(handle) ? 1 : 0, closing_now());
    EXPECT_EQ(failed, 0);
    EXPECT_TRUE(open_after_the_drain) << "the budget cut a peer that had only filled the socket buffers and stopped";
    EXPECT_EQ(got.received, LAST);
    EXPECT_STREQ(got.end, "EOF") << "a peer that stops when it reads EOF must end without a reset";
    EXPECT_FALSE(descriptor_open(handle));
    EXPECT_EQ(closing_now(), 0u);
}

// F4 -- the node's construction is three allocations (the node, its read watcher's record, its timer's). Each in turn
// fails, on a thread of its own (fresh pools): the close must fall back to "at once" and leave nothing registered.
TEST_F(CvClose, F4AFailedAllocationOfTheNodeClosesAtOnceAndLeavesNothing) {
    struct Result {
        bool        ran = false, fired = false, open_after = true, eof = false;
        int         allocations   = 0;
        std::size_t closing_after = 99, registered_before = 0, registered_after = 0, closing_next = 99;
    };
    for (int k = 0; k < 4; ++k) {
        Result      r;
        std::thread worker([&r, k] {
            qb::io::async::init();
            {
                Client client;
                Pair   pair;
                if (pair.listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
                    return;
                if (client.transport().connect_v4("127.0.0.1", pair.listener.local_endpoint().port()) != qb::io::SocketStatus::Done)
                    return;
                if (pair.listener.accept(pair.peer) != qb::io::SocketStatus::Done)
                    return;
                client.start();
                const auto handle     = client.transport().native_handle();
                client.arm_allocation = k;
                client.disconnect();
                r.registered_before = qb::io::async::listener::current.size();
                while (client.dispose_events == 0u)
                    qb::io::async::run(EVRUN_NOWAIT);
                r.fired            = client.injection_fired;
                r.allocations      = client.allocations_between;
                r.open_after       = descriptor_open(handle);
                r.closing_after    = closing_now();
                r.registered_after = qb::io::async::listener::current.size();
                r.eof              = std::string_view{read_all(pair.peer, 300ms, true).end} == "EOF";
                // the next close on this thread works as usual
                Client next;
                Pair   next_pair;
                if (next_pair.listener.listen_v4(0, "127.0.0.1") == qb::io::SocketStatus::Done
                    && next.transport().connect_v4("127.0.0.1", next_pair.listener.local_endpoint().port()) == qb::io::SocketStatus::Done
                    && next_pair.listener.accept(next_pair.peer) == qb::io::SocketStatus::Done) {
                    next.start();
                    next.disconnect();
                    while (next.dispose_events == 0u)
                        qb::io::async::run(EVRUN_NOWAIT);
                    r.closing_next = closing_now();
                }
                r.ran = true;
            }
            qb::io::async::listener::current.clear();
        });
        worker.join();
        std::printf("CV-W F4 [%s] allocation #%d fails: injection fired=%d (allocations counted while armed=%d); descriptor open after the "
                    "dispose=%d closing=%zu; watchers registered before=%zu after=%zu; the peer reads EOF=%d; the next close on the thread: "
                    "closing=%zu\n",
                    tree(), k + 1, r.fired ? 1 : 0, r.allocations, r.open_after ? 1 : 0, r.closing_after, r.registered_before,
                    r.registered_after, r.eof ? 1 : 0, r.closing_next);
        ASSERT_TRUE(r.ran);
        if (k < 3) {
            EXPECT_TRUE(r.fired) << "allocation #" << k + 1 << " was never reached: the node is not made of three";
            EXPECT_FALSE(r.open_after) << "the descriptor of a node that could not be made stays open";
            EXPECT_EQ(r.closing_after, 0u);
            EXPECT_EQ(r.registered_after, r.registered_before) << "a watcher of the failed node is still registered";
            EXPECT_TRUE(r.eof);
        } else {
            EXPECT_FALSE(r.fired) << "a fourth allocation exists between the hooks";
            EXPECT_EQ(r.closing_after, 1u);
        }
        EXPECT_EQ(r.closing_next, k < 3 ? 1u : 2u);
    }
}

// F5 -- the last read before the final close, and what it is for. The node ends with input unread (the loop is cleared
// before it ran again) while octets this side wrote are still in ITS OWN send queue: the peer has read nothing, its
// receive buffer is full, the rest waits here behind it, the FIN last. A close with input unread aborts the connection
// and drops that queue (Linux: net/ipv4/tcp.c, tcp_close(); the dossier's citation); a close after one last read is an
// ordinary close, and the system goes on delivering. So: the peer, when it finally reads, must get every octet the
// socket took, then a clean EOF. (What the peer had ALREADY received survives a reset that follows our FIN -- the TCP
// witness's K9, and the first form of this case, which the mutant passed on Windows: the last read is not about that.)
TEST_F(CvClose, F5TheLastReadBeforeTheFinalCloseKeepsWhatIsStillToBeDelivered) {
    Client client;
    Pair   pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
    const std::string payload(64 * 1024, 'd');
    for (int i = 0; i < 4096 && client.out().size() < 4 * payload.size(); ++i) { // until the socket takes no more
        client.publish(std::string_view{payload});
        qb::io::async::run(EVRUN_NOWAIT);
        qb::io::async::run(EVRUN_NOWAIT);
    }
    pump(50ms);
    const std::size_t left_in_the_object = client.out().size(); // dropped with the object's output, as before
    client.disconnect();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return client.disconnected_events == 1u; }));
    const std::size_t       taken          = client.bytes_written(); // what the socket took: all of it must arrive
    const std::size_t       closing_before = closing_now();
    const std::vector<char> late(4096, 'l');
    ASSERT_EQ(pair.peer.write(late.data(), late.size()), 4096); // arrives; no loop pass reads it
    std::this_thread::sleep_for(50ms);
    qb::io::async::listener::current.clear(); // the node ends here, its input unread
    std::this_thread::sleep_for(100ms);
    const Read got = read_all(pair.peer, 3000ms, false);
    std::printf("CV-W F5 [%s] the socket took %zu octets the peer had not read (%zu more stayed in the object); closing before "
                "clear()=%zu after=%zu; the peer then read %zu octets then %s (error %d)\n",
                tree(), taken, left_in_the_object, closing_before, closing_now(), got.received.size(), got.end, got.error);
    EXPECT_EQ(closing_before, 1u);
    EXPECT_EQ(got.received.size(), taken) << "the final close dropped what was still to be delivered";
    EXPECT_STREQ(got.end, "EOF") << "the node closed with input unread: a reset";
}

// F6a -- D3 as stated: a blocking run() after the last connection ended returns within linger() when the peer is silent...
TEST_F(CvClose, F6aABlockingRunReturnsWithinTheBound) {
    double silent_ms = -1, zero_ms = -1, closing_ms = -1;
    {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(300ms);
        client.disconnect();
        const auto start = std::chrono::steady_clock::now();
        qb::io::async::run();
        silent_ms = ms_since(start);
    }
    {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.set_linger(qb::duration::zero()); // ...at once with a linger of zero...
        client.disconnect();
        const auto start = std::chrono::steady_clock::now();
        qb::io::async::run();
        zero_ms = ms_since(start);
    }
    {
        Client client;
        Pair   pair;
        ASSERT_NO_FATAL_FAILURE(connect_pair(client, pair));
        client.disconnect();
        qb::io::async::run(EVRUN_NOWAIT);
        qb::io::async::run(EVRUN_NOWAIT);
        pair.peer.close(); // ...and at once when the peer closes.
        const auto start = std::chrono::steady_clock::now();
        qb::io::async::run();
        closing_ms = ms_since(start);
    }
    std::printf("CV-W F6a [%s] a blocking run() after disconnect(): silent peer, linger 300 ms: %.0f ms; linger 0: %.0f ms; a peer that "
                "closes: %.0f ms\n",
                tree(), silent_ms, zero_ms, closing_ms);
    EXPECT_LE(silent_ms, 300. + 250.);
    EXPECT_LE(zero_ms, 100.);
    EXPECT_LE(closing_ms, 100.);
    EXPECT_EQ(closing_now(), 0u);
}

// F6b -- ...and a thread that stopped running its loop keeps the descriptor until one pass runs: the bound is the loop's.
TEST_F(CvClose, F6bOneLoopPassClosesWhatAnIdleThreadKept) {
    auto client = std::make_unique<Client>();
    Pair pair;
    ASSERT_NO_FATAL_FAILURE(connect_pair(*client, pair));
    const auto handle = client->transport().native_handle();
    client->set_linger(100ms);
    client->disconnect();
    while (client->disconnected_events == 0u)
        qb::io::async::run(EVRUN_NOWAIT);
    client.reset();
    std::this_thread::sleep_for(300ms); // past the bound, no loop pass
    const bool open_idle = descriptor_open(handle);
    qb::io::async::run(EVRUN_NOWAIT);
    const bool open_after_pass = descriptor_open(handle);
    std::printf("CV-W F6b [%s] linger 100 ms, 300 ms without a loop pass: descriptor open=%d; after one pass: open=%d closing=%zu\n", tree(),
                open_idle ? 1 : 0, open_after_pass ? 1 : 0, closing_now());
    EXPECT_FALSE(open_after_pass) << "the pass that follows the bound must close it";
    EXPECT_EQ(closing_now(), 0u);
}
#endif // CV_HAS_CLOSER
