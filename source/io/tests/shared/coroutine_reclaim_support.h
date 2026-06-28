/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/coroutine_reclaim_support.h
 * @brief Shared harness for the "destroy-while-parked" use-after-free regression that every
 *        coroutine awaiter must withstand. Each primitive's reclaim test lives in that primitive's
 *        own test file (sync-primitives, combinators, generator, shared-task, scope, cancellation);
 *        this header is the one piece of machinery they all share.
 *
 * The scenario: a coroutine suspends inside an awaiter that stored its `coroutine_handle` (or a
 * `continuation`) in some external list it does not own; its frame is then destroyed *while still
 * parked* — canonical trigger: a `when_any`/`race` loser-branch reclamation. A later signal
 * (`unlock`/`release`/`set`/`count_down`/a `co_yield`/a sibling branch finishing) then resumes (or,
 * for `semaphore::release`, writes through) that freed frame → heap-use-after-free. The fix is a
 * retracting destructor on every such awaiter (mirroring channel.h).
 *
 * Each reclaim test pre-arms a resource so a parker must suspend, races the parker against
 * `reclaim_fast_winner()` via `when_any` (the winner wins → the parker's frame is reclaimed while
 * parked), then fires the resource's wake. With the fix these run clean; without it each is a
 * heap-use-after-free under ASan — and the `g_resumed_after_reclaim` guard catches a stray resume
 * even without a sanitizer.
 *
 * Header-only; all helpers live in namespace `qb::io::test`. Drives `coro_scheduler().spawn()` + the
 * shared bounded pump, so a hosting test only needs the usual `reset_async_context()` SetUp.
 */

#ifndef QB_IO_TESTS_SHARED_COROUTINE_RECLAIM_SUPPORT_H
#define QB_IO_TESTS_SHARED_COROUTINE_RECLAIM_SUPPORT_H

#include <atomic>
#include <chrono>
#include <memory>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "coroutine_test_support.h"

namespace qb::io::test {

// Set true only if a reclaimed parker's body runs *past* its suspend point — i.e. a dangling resume
// fired. Must stay false in every reclaim test (the parker loses the race and is reclaimed parked).
// inline → one shared instance per test binary (the parker writes it, the driver reads it).
inline std::atomic<bool> g_resumed_after_reclaim{false};

// Wins the when_any race so the parker's frame is reclaimed while still parked.
inline qb::io::async::task<int>
reclaim_fast_winner() {
    co_await qb::io::async::sleep(std::chrono::milliseconds(5));
    co_return 99;
}

// Reset the guard, spawn the driver, pump to completion, then assert it ran AND that no
// reclaimed-while-parked parker was spuriously resumed (a dangling handle).
template <typename DriverFactory>
void
run_reclaim_driver(DriverFactory make_driver) {
    g_resumed_after_reclaim.store(false, std::memory_order_relaxed);
    auto done = std::make_shared<std::atomic<bool>>(false);
    qb::io::async::coro_scheduler().spawn(
        [](std::shared_ptr<std::atomic<bool>> d, DriverFactory mk) -> qb::io::async::task<void> {
            co_await mk();
            d->store(true, std::memory_order_relaxed);
            co_return;
        }(done, std::move(make_driver)));
    EXPECT_TRUE(pump_until([&] { return done->load(std::memory_order_relaxed); }))
        << "reclaim driver coroutine never completed";
    EXPECT_FALSE(g_resumed_after_reclaim.load(std::memory_order_relaxed))
        << "a reclaimed-while-parked coroutine was resumed (dangling handle)";
}

} // namespace qb::io::test

// QB_RECLAIM_PARK(EXPR): the whole body of a parker coroutine. Suspends on EXPR with an oversized
// (>4 KiB) local kept live across the suspend so the frame escapes the coroutine frame-allocator
// pool (otherwise a pooled frame is never returned to the allocator and ASan cannot see the free).
// The post-suspend writes only execute on a (buggy) resume → they trip g_resumed_after_reclaim.
#define QB_RECLAIM_PARK(EXPR)                                                                       \
    volatile char big[8192];                                                                        \
    big[0] = 7;                                                                                      \
    co_await (EXPR);                                                                                 \
    big[1] = big[0];                                                                                 \
    ::qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);                  \
    co_return (int) big[1];

#endif // QB_IO_TESTS_SHARED_COROUTINE_RECLAIM_SUPPORT_H
