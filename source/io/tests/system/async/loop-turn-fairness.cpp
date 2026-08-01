/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/loop-turn-fairness.cpp
 * @brief One `listener::run()` turn must always end, whatever the coroutines are doing.
 *
 * `CoroutineScheduler::run_ready()` drains until its ready queue is empty, and every waker in
 * qb-io defers through `schedule_via_current`, which pushes straight onto that queue. So a
 * coroutine resumed *from* the drain can enqueue the next one *during* the drain. Two coroutines
 * that resume each other — an unbuffered `channel<T>` producer/consumer pipeline with no I/O
 * await in the cycle is enough, and that is an ordinary shape — then keep the queue permanently
 * non-empty.
 *
 * With an unbounded drain that means `listener::run()` **never returns**. Nothing else in the
 * turn runs again: no libev watcher, and — decisively — a `VirtualCore` driving this listener
 * never reaches `__flush_all__`, `__receive__` or its actor callbacks. The engine deadlocks
 * while burning a core. Measured before the bound: a single `run(EVRUN_NOWAIT)` turn executed
 * **2,000,000** ping-pongs in 162 ms, and only returned because the probe's loops were finite.
 *
 * `listener::run()` therefore caps resumes per turn (`kMaxCoroutineResumesPerTurn`). The cap is
 * per turn, not per coroutine: work scheduled past it simply runs on the next turn, so nothing
 * is dropped or reordered.
 *
 * This test pins the property that matters — **the turn ends, and it ends early** — rather than
 * the exact constant. It is deliberately shaped so a regression fails instead of hanging: the
 * ping-pong is finite, and a broken build simply drains all of it in one turn.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <qb/io/async.h>

using namespace qb::io::async;

namespace {

/// Far more work than one turn may legitimately absorb, but finite so a regression cannot hang.
constexpr long kPingPongs = 4'000'000;

std::atomic<long> g_completed{0};

} // namespace

TEST(LoopTurnFairness, MutuallyResumingCoroutinesCannotMonopoliseATurn) {
    qb::io::async::init();
    g_completed.store(0, std::memory_order_relaxed);

    auto ch = std::make_shared<qb::io::async::channel<int>>(0); // unbuffered: every send needs a recv

    auto producer = [](std::shared_ptr<qb::io::async::channel<int>> c) -> task<void> {
        for (long i = 0; i < kPingPongs; ++i) {
            co_await c->send(1);
            g_completed.fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };
    auto consumer = [](std::shared_ptr<qb::io::async::channel<int>> c) -> task<void> {
        for (long i = 0; i < kPingPongs; ++i) {
            auto v = co_await c->recv();
            if (!v)
                break;
        }
        co_return;
    };

    coro_scheduler().spawn(producer(ch));
    coro_scheduler().spawn(consumer(ch));

    const auto start = std::chrono::steady_clock::now();
    listener::current.run(EVRUN_NOWAIT); // exactly one turn
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    const long done = g_completed.load(std::memory_order_relaxed);

    EXPECT_GT(done, 0) << "the turn resumed nothing — the harness is not exercising the drain at all";
    EXPECT_LT(done, kPingPongs) << "a single event-loop turn drained the ENTIRE ping-pong (" << done
                                << "): the coroutine drain is unbounded again, so two mutually-resuming "
                                   "coroutines can keep run() from ever returning and starve the core";
    // Generous: the point is that the turn is bounded, not that it is fast on a loaded machine.
    EXPECT_LT(elapsed.count(), 5000) << "one turn took " << elapsed.count() << " ms";

    ch->close(); // let the parked coroutines unwind
    listener::current.run(EVRUN_NOWAIT);
}
