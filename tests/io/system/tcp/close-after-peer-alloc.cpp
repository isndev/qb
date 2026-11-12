/**
 * @file qb/tests/io/system/tcp/close-after-peer-alloc.cpp
 * @brief A close after the peer whose loop-owned node cannot be allocated closes at once and leaves nothing
 *        registered (Huly QB-1270).
 *
 * `detail::close_after_peer` is `noexcept` and runs inside `dispose()`: when the peer has not closed yet it makes one
 * node -- the node itself, the record of its read watcher, the record of its timer: three allocations. Whichever
 * fails, the descriptor must be closed at once (no node, no linger: what is left), no watcher record may stay
 * registered on the loop -- a read watcher registered before a timer that could not be made is the gap the first
 * prototype had -- and the next close on the thread must work as usual.
 *
 * Alone in its binary: the case replaces the global allocation functions to make the Nth allocation between the two
 * teardown hooks fail.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup IO
 */

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <thread>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/close_after_peer_support.h"

using namespace std::chrono_literals;

// The Nth allocation of the calling thread fails, while armed. Every form of the global allocation functions is
// replaced, the nothrow and the array ones included: a sanitizer's runtime supplies its own for whatever form is left
// out, and a block one of those allocated must not reach free().
namespace close_after_peer_alloc_test::injection {

thread_local int  countdown = -1;    ///< -1: off; otherwise the allocations left before the one that fails.
thread_local int  seen      = 0;     ///< Allocations seen while armed.
thread_local bool fired     = false; ///< Whether the armed allocation was reached, and failed.

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

} // namespace close_after_peer_alloc_test::injection

// clang-format off
namespace inj = close_after_peer_alloc_test::injection;
void *operator new(std::size_t n) { return inj::plain(n); }
void *operator new[](std::size_t n) { return inj::plain(n); }
void *operator new(std::size_t n, std::nothrow_t const &) noexcept { return inj::or_null([n] { return inj::plain(n); }); }
void *operator new[](std::size_t n, std::nothrow_t const &) noexcept { return inj::or_null([n] { return inj::plain(n); }); }
void *operator new(std::size_t n, std::align_val_t a) { return inj::aligned(n, a); }
void *operator new[](std::size_t n, std::align_val_t a) { return inj::aligned(n, a); }
void *operator new(std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return inj::or_null([n, a] { return inj::aligned(n, a); }); }
void *operator new[](std::size_t n, std::align_val_t a, std::nothrow_t const &) noexcept { return inj::or_null([n, a] { return inj::aligned(n, a); }); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete[](void *p, std::nothrow_t const &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { inj::release_aligned(p); }
void operator delete(void *p, std::align_val_t, std::nothrow_t const &) noexcept { inj::release_aligned(p); }
void operator delete[](void *p, std::align_val_t, std::nothrow_t const &) noexcept { inj::release_aligned(p); }
// clang-format on

namespace close_after_peer_alloc_test {

using namespace qb::io::test::close_after_peer;

/// What one thread saw of a close whose `failing`-th allocation (from 0) between the two hooks was made to fail.
struct Outcome {
    bool        ran               = false; ///< The thread reached its end.
    bool        fired             = false; ///< The armed allocation was reached.
    int         allocations       = 0;     ///< Allocations counted while armed, the failed one included.
    bool        open_after        = true;  ///< The descriptor, read inside on(event::dispose&&).
    std::size_t closing_after     = 99;    ///< closing_sockets(), read inside on(event::dispose&&).
    std::size_t registered_before = 0;     ///< Watcher records on the loop before the dispose.
    std::size_t registered_after  = 0;     ///< ...and after it.
    bool        peer_read_eof     = false; ///< The peer reads a clean EOF.
    std::size_t closing_next      = 99;    ///< closing_sockets() after the NEXT close on the same thread.
};

Outcome
close_with_a_failing_allocation(int failing) {
    Outcome     outcome;
    std::thread worker([&outcome, failing] { // a thread of its own: fresh watcher-record pools, so each record is an allocation
        qb::io::async::init();
        {
            Client client;
            Pair   pair;
            if (pair.listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done
                || client.transport().connect_v4("127.0.0.1", pair.listener.local_endpoint().port()) != qb::io::SocketStatus::Done
                || pair.listener.accept(pair.peer) != qb::io::SocketStatus::Done)
                return;
            client.set_linger(20s);
            client.start();
            // Armed between the two hooks: what runs there is the watcher's stop and detail::end_transport.
            client.disconnected_hook = [failing] {
                injection::seen      = 0;
                injection::fired     = false;
                injection::countdown = failing;
            };
            client.dispose_hook = [&outcome] {
                outcome.allocations  = injection::seen;
                outcome.fired        = injection::fired;
                injection::countdown = -1;
            };
            client.disconnect();
            outcome.registered_before = qb::io::async::listener::current.size();
            while (client.dispose_events == 0u)
                qb::io::async::run(EVRUN_NOWAIT);
            outcome.open_after       = client.descriptor_open_in_dispose;
            outcome.closing_after    = client.closing_in_dispose;
            outcome.registered_after = qb::io::async::listener::current.size();
            outcome.peer_read_eof    = read_all(pair.peer, 2000ms).end == "EOF";

            Client next; // the next close on this thread works as usual
            Pair   next_pair;
            if (next_pair.listener.listen_v4(0, "127.0.0.1") == qb::io::SocketStatus::Done
                && next.transport().connect_v4("127.0.0.1", next_pair.listener.local_endpoint().port()) == qb::io::SocketStatus::Done
                && next_pair.listener.accept(next_pair.peer) == qb::io::SocketStatus::Done) {
                next.set_linger(20s);
                next.start();
                next.disconnect();
                while (next.dispose_events == 0u)
                    qb::io::async::run(EVRUN_NOWAIT);
                outcome.closing_next = closing();
            }
            outcome.ran = true;
        }
        qb::io::async::listener::current.clear();
    });
    worker.join();
    return outcome;
}

} // namespace close_after_peer_alloc_test

using namespace close_after_peer_alloc_test;

// Each of the node's three allocations fails in turn.
TEST(CloseAfterPeerAlloc, ANodeThatCannotBeAllocatedClosesAtOnceAndLeavesNothingRegistered) {
    for (int failing = 0; failing < 3; ++failing) {
        SCOPED_TRACE("allocation #" + std::to_string(failing + 1) + " of the node fails");
        const Outcome outcome = close_with_a_failing_allocation(failing);
        ASSERT_TRUE(outcome.ran);
        EXPECT_TRUE(outcome.fired) << "the allocation was never reached (" << outcome.allocations << " counted): the node is not made of three";
        EXPECT_FALSE(outcome.open_after) << "the descriptor of a node that could not be made stays open";
        EXPECT_EQ(outcome.closing_after, 0u);
        EXPECT_EQ(outcome.registered_after, outcome.registered_before) << "a watcher record of the failed node is still registered";
        EXPECT_TRUE(outcome.peer_read_eof);
        EXPECT_EQ(outcome.closing_next, 1u) << "the next close on the thread did not go to the loop";
    }
}

// The control: there is no fourth allocation between the hooks, so the injection armed for it never fires and the
// close is the ordinary one -- which is what makes the three above a complete account.
TEST(CloseAfterPeerAlloc, TheNodeIsMadeOfThreeAllocations) {
    const Outcome outcome = close_with_a_failing_allocation(3);
    ASSERT_TRUE(outcome.ran);
    EXPECT_FALSE(outcome.fired) << "a fourth allocation exists between the hooks (" << outcome.allocations << " counted)";
    EXPECT_EQ(outcome.allocations, 3);
    EXPECT_TRUE(outcome.open_after) << "the peer has not closed: the loop holds the descriptor";
    EXPECT_EQ(outcome.closing_after, 1u);
    EXPECT_EQ(outcome.closing_next, 2u);
}
