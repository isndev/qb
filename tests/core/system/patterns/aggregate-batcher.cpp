/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/aggregate-batcher.cpp
 * @brief Size/time-windowed batching: `qb::batcher<T>` count trigger, window trigger, scope-bound
 *        cancel, and manual `flush()` drain.
 *
 * `batcher` coalesces a stream of items and flushes the whole vector when EITHER `max` items
 * accumulate OR `window` elapses since the first buffered item — whichever comes first. The window
 * timer is a cancellation-aware coroutine bound to the actor scope, so a killed actor drops the
 * pending flush (buffered items are NOT flushed) unless a shutdown handler calls `flush()`. Proven
 * here against the running engine:
 *   - COUNT trigger: 7 items with max 3 flush at 3 and at 6 (exactly 2 flushes, 6 items), item #7
 *     stays buffered (the long window never fires) — `pending()` proves the residue;
 *   - WINDOW trigger: 2 items below the count threshold flush once when the time window elapses;
 *   - KILL: items are genuinely buffered (pending == 2) and the actor is killed before the long
 *     window → the scope-bound timer is cancelled → ZERO flushes (dropped, not flushed) and join()
 *     does not hang;
 *   - MANUAL flush(): drains the buffer immediately and a second flush() on an empty buffer is a
 *     no-op (no double-flush).
 *
 * De-flaked vs the monolith: each run's shutdown is event-driven — the count/window/manual cases
 * stop the engine from inside the flush callback (the observable completion), and the kill cases
 * self-kill (the engine empties and stops naturally). No fixed wall-clock `stop()` offset is the
 * oracle. The window length is the SUT itself, not an ordering oracle, with a generous ctest
 * timeout as the only wall-clock backstop.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>
#include <vector>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int> g_batch_flushes{0};
std::atomic<int> g_batch_items{0};
std::atomic<int> g_batch_pending{-1};

void
reset_batch() {
    g_batch_flushes.store(0);
    g_batch_items.store(0);
    g_batch_pending.store(-1);
}
} // namespace

// ===========================================================================
// Count trigger: flushes synchronously every `max` items.
// ===========================================================================
class BatchCountActor : public qb::Actor {
    qb::batcher<int> _batch{3, 5s, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 7; ++i)
            _batch.add(context(), i); // flushes synchronously at 3 and at 6
        g_batch_pending.store(static_cast<int>(_batch.pending()));
        kill(); // synchronous count flushes already happened → engine empties and stops
        co_return true;
    }
};

TEST(Batcher, FlushesOnCount) {
    reset_batch();
    qb::Main main;
    main.addActor<BatchCountActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_flushes.load(), 2) << "7 items, max 3 → flush at 3 and at 6";
    EXPECT_EQ(g_batch_items.load(), 6);
    EXPECT_EQ(g_batch_pending.load(), 1) << "item #7 still buffered (the 5s window did not fire)";
}

// ===========================================================================
// Window trigger: items below max flush when the time window elapses.
// ===========================================================================
class BatchWindowActor : public qb::Actor {
    qb::batcher<int> _batch{100, 30ms, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                                qb::Main::stop(); // event-driven: the window flush IS the completion
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // arms the 30ms window timer
        _batch.add(context(), 2); // same batch, count (2) below max (100)
        co_return true;
    }
};

TEST(Batcher, FlushesOnWindow) {
    reset_batch();
    qb::Main main;
    main.addActor<BatchWindowActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_flushes.load(), 1) << "count never hit → the time window flushed exactly once";
    EXPECT_EQ(g_batch_items.load(), 2);
}

// ===========================================================================
// Kill before the window: the scope-bound timer is cancelled → buffered items dropped.
// ===========================================================================
class BatchKillActor : public qb::Actor {
    qb::batcher<int> _batch{100, 5s, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // arms a 5s scope-bound timer
        _batch.add(context(), 2);
        g_batch_pending.store(static_cast<int>(_batch.pending())); // proof: 2 items really buffered
        // Self-kill before the window: the actor dies → the scope-bound timer is cancelled → engine
        // empties and stops. No lingering stop-callback that could fire into a later test.
        //
        // Deliberate, and NOT the `[this]`-outlives-the-actor hazard: this callback is the actor's
        // ONLY death, so the core loop cannot empty — and the listener cannot tear the pending
        // Timeout down — before it fires; `is_alive()` never reads freed memory here. The trigger
        // also stays deliberately OUT-OF-BAND: what this test measures is the scope-bound batcher
        // timer dying with the actor, so the kill is kept outside that scope. Do not convert.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            40ms);
        co_return true;
    }
};

TEST(Batcher, KillCancelsPendingFlush) {
    reset_batch();
    qb::Main main;
    main.addActor<BatchKillActor>(0);
    main.start(false);
    main.join(); // must not hang; the scope-bound window timer is cancelled on kill
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_pending.load(), 2) << "items were buffered (teeth: not a no-op)";
    EXPECT_EQ(g_batch_flushes.load(), 0) << "killed before the window → dropped, not flushed";
}

// ===========================================================================
// Manual flush(): documented shutdown-drain path; a second flush() is an empty no-op.
// ===========================================================================
class BatchManualFlushActor : public qb::Actor {
    qb::batcher<int> _batch{100, 5s, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // below the count threshold, long window → no auto-flush
        _batch.add(context(), 2);
        _batch.add(context(), 3);
        _batch.flush(); // manual drain — flushes the 3 buffered items now
        _batch.flush(); // no-op on an empty buffer (must not double-flush)
        kill();         // drains already happened → engine empties and stops
        co_return true;
    }
};

TEST(Batcher, ManualFlushDrains) {
    reset_batch();
    qb::Main main;
    main.addActor<BatchManualFlushActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_flushes.load(), 1) << "exactly one flush (the second flush() was an empty no-op)";
    EXPECT_EQ(g_batch_items.load(), 3) << "all three buffered items drained";
}
