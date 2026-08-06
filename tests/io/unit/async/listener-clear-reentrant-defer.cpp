/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/listener-clear-reentrant-defer.cpp
 * @brief `listener::clear()` must drop the deferred queue by SWAPPING it out, never by clearing
 *        it in place — a released closure's destructor can `defer()` again.
 *
 * `clear()` drops every pending `async::defer()` closure without running it. Dropping a closure
 * runs its captured state's destructors, and that is arbitrary user code: a captured `shared_ptr`
 * can be the last reference to an object whose teardown calls `defer()` again. `tcp::connector`
 * defers a closure capturing a `shared_ptr` to itself, and qbm-http's HTTP/1.1 client does the same
 * for its per-turn self-hold, so `clear()` really can be the call that runs `~Client`.
 *
 * A `push_back` into a `std::deque` that is halfway through its own `clear()` is undefined.
 *
 * WHAT THIS TEST ASSERTS, AND WHY IT IS NOT THE OBVIOUS THING. The tempting assertion is "nothing
 * leaks": destroy the queue, count captured state, require it back at zero. That assertion does not
 * discriminate, and this file used to make it — with the fix reverted it stayed green on libc++,
 * because whether the re-entrant element is reclaimed or abandoned is an implementation detail of
 * `deque::clear()`, and one implementation happens to pick it up. A test whose outcome depends on
 * which standard library is present is not a test of qb.
 *
 * The invariant that IS qb's, and that holds on every implementation, is the one the fix
 * establishes: **the member queue must be empty while its closures' destructors run**. That is what
 * makes a re-entrant `defer()` safe rather than undefined — it lands in a fresh, untouched deque
 * instead of the one mid-destruction. So the probe below reports `has_deferred()` from inside a
 * destructor that `clear()` is running, and requires it to be false. Before the fix the member IS
 * the container being cleared and still reports its remaining elements; after it, `clear()` has
 * already swapped the queue into a local. Verified both ways.
 */

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <gtest/gtest.h>
#include <qb/io/async.h>

namespace {

/// Counts live instances of a closure's captured state.
struct Tracer {
    static inline std::atomic<int> live{0};
    static inline std::atomic<int> made{0};

    Tracer() {
        live.fetch_add(1, std::memory_order_relaxed);
        made.fetch_add(1, std::memory_order_relaxed);
    }
    Tracer(Tracer const &) {
        live.fetch_add(1, std::memory_order_relaxed);
        made.fetch_add(1, std::memory_order_relaxed);
    }
    ~Tracer() {
        live.fetch_sub(1, std::memory_order_relaxed);
    }

    static void
    reset() {
        live.store(0, std::memory_order_relaxed);
        made.store(0, std::memory_order_relaxed);
    }
};

std::atomic<bool> g_ran{false};              ///< the probe's destructor really ran under clear()
std::atomic<bool> g_saw_member_queue{false}; ///< …and what it saw the member queue holding
std::atomic<bool> g_redeferred{false};       ///< …and its own re-entrant defer was accepted

/// Held by a deferred closure through a `shared_ptr`, so `clear()` releasing that closure is what
/// destroys it — the `tcp::connector` / qbm-http self-hold shape.
struct Probe {
    ~Probe() {
        if (!g_ran.exchange(true)) {
            // Read BEFORE re-deferring: this is the whole measurement. `clear()` is running our
            // destructor, so the queue it is dropping must no longer be the member.
            g_saw_member_queue.store(qb::io::async::listener::current.has_deferred(), std::memory_order_relaxed);
        }
        qb::io::async::defer([tag = Tracer{}] { (void) tag; });
        g_redeferred.store(true, std::memory_order_relaxed);
    }
};

/// Queue `n` closures that do nothing but occupy the deque, so the member is demonstrably
/// non-empty at the moment the probe's destructor runs.
void
queue_filler(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        qb::io::async::defer([tag = Tracer{}] { (void) tag; });
}

void
arm(std::size_t before, std::size_t after) {
    g_ran.store(false, std::memory_order_relaxed);
    g_saw_member_queue.store(false, std::memory_order_relaxed);
    g_redeferred.store(false, std::memory_order_relaxed);
    Tracer::reset();

    queue_filler(before);
    {
        auto probe = std::make_shared<Probe>();
        qb::io::async::defer([probe, tag = Tracer{}] {
            (void) probe;
            (void) tag;
        });
    }
    queue_filler(after);
}

} // namespace

TEST(ListenerClear, MemberQueueIsEmptyWhileDroppedClosuresRunTheirDestructors) {
    qb::io::async::init();
    qb::io::async::listener::current.clear(); // start from a known-empty queue

    // Fillers on BOTH sides of the probe, so the member queue is non-empty when the probe's
    // destructor runs whichever end `deque::clear()` starts from.
    arm(/*before=*/3, /*after=*/3);
    ASSERT_TRUE(qb::io::async::listener::current.has_deferred()) << "nothing was queued";

    qb::io::async::listener::current.clear();

    ASSERT_TRUE(g_ran.load()) << "clear() never released the probe closure, so nothing was measured";
    ASSERT_TRUE(g_redeferred.load()) << "the probe's destructor never reached its defer(), so the re-entrant path was not exercised";

    EXPECT_FALSE(g_saw_member_queue.load()) << "a closure's destructor observed clear() still holding its queue in the listener's own "
                                               "member. Anything that destructor defers is a push_back into a std::deque that is "
                                               "halfway through its own clear() — undefined. clear() must swap the queue into a local "
                                               "before dropping it (listener::clear()).";

    EXPECT_FALSE(qb::io::async::listener::current.has_deferred())
        << "clear() returned with work still queued: the drain does not loop, so a closure deferred "
           "while the queue was being dropped survives into a torn-down listener";
}

TEST(ListenerClear, RepeatedClearIsIdempotentAndLeavesNothingLive) {
    qb::io::async::init();
    qb::io::async::listener::current.clear();

    arm(/*before=*/2, /*after=*/2);
    ASSERT_TRUE(qb::io::async::listener::current.has_deferred());

    qb::io::async::listener::current.clear();
    EXPECT_FALSE(qb::io::async::listener::current.has_deferred());
    EXPECT_EQ(Tracer::live.load(), 0) << "captured state outlived the queue it was stored in";

    qb::io::async::listener::current.clear(); // a second clear must be a no-op, not a fault
    EXPECT_FALSE(qb::io::async::listener::current.has_deferred());
    EXPECT_EQ(Tracer::live.load(), 0);
}
