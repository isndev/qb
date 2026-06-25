/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/routing-dispatch.cpp
 * @brief `qb::WorkerPool` dispatch under the live engine — round-robin even distribution and
 *        dispatch-to-an-Activating-worker (the mailbox stash/replay path).
 *
 * The pure-logic contract of `WorkerPool` (cursor arithmetic, for_key, resize) is owned by
 * unit/patterns/routing-worker-pool.cpp. This SYSTEM file proves the *runtime* consequence of
 * that arithmetic: a `Dispatcher` actor that `push<Job>(pool.next(), …)` actually lands the
 * jobs on the workers' mailboxes with the exact round-robin shape, on a running `qb::Main`.
 *
 * What is proven:
 *   - `RoundRobinDistributesEvenly`: 9 jobs over 3 workers via `pool.next()` arrive as exactly
 *     3-3-3 — the framework-observable proof that `next()` cycles in delivery order, not merely
 *     in the value returned. The shutdown is event-driven: the Nth job stops the engine.
 *   - `DispatchToActivatingWorkersStashesAndReplays`: the workers' async `onInit` is still
 *     suspended (Activating) when the Dispatcher fans the jobs out. Every job must be stashed at
 *     its target and replayed once the worker activates — asserted by an EXACT total of `kJobs`
 *     and by every worker having received at least one (nothing silently dropped).
 *
 * In-actor work is guarded by a post-`join()` `g_dispatcher_ran` flag so a never-scheduled
 * Dispatcher cannot make a count assertion pass vacuously. A generous ctest TIMEOUT is the only
 * backstop; no wall-clock EXPECT is used as an oracle.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor suites.
 */

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <vector>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

namespace {

constexpr int kWorkers = 3;
constexpr int kJobs    = 9; // 9 / 3 == 3 jobs per worker, exactly

std::atomic<int>  g_worker_recv[kWorkers];
std::atomic<int>  g_jobs_done{0};
std::atomic<bool> g_dispatcher_ran{false}; // set at the end of Dispatcher::onInit (ran-guard)

void
reset_counters() {
    for (auto &c : g_worker_recv)
        c.store(0, std::memory_order_relaxed);
    g_jobs_done.store(0, std::memory_order_relaxed);
    g_dispatcher_ran.store(false, std::memory_order_relaxed);
}

} // namespace

struct Job : public qb::Event {
    int n{0};
    explicit Job(int v)
        : n(v) {}
};

// A worker that counts the jobs it receives; the Nth job overall stops the engine (event-driven
// completion — no sleep, no fixed-ms offset used as an ordering oracle).
class Worker : public qb::Actor {
    const int _idx;

public:
    explicit Worker(int idx)
        : _idx(idx) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Job>(*this);
        co_return true;
    }
    void
    on(Job &) {
        g_worker_recv[_idx].fetch_add(1, std::memory_order_relaxed);
        if (g_jobs_done.fetch_add(1, std::memory_order_relaxed) + 1 == kJobs)
            qb::Main::stop();
    }
};

// Same as Worker but its onInit suspends (Activating) so jobs land while it is not yet ready —
// exercising the per-actor mailbox stash + replay-on-activation path.
class ActivatingWorker : public qb::Actor {
    const int _idx;

public:
    explicit ActivatingWorker(int idx)
        : _idx(idx) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Job>(*this);
        co_await context().sleep(25ms); // still Activating while the Dispatcher fans jobs out
        co_return true;
    }
    void
    on(Job &) {
        g_worker_recv[_idx].fetch_add(1, std::memory_order_relaxed);
        if (g_jobs_done.fetch_add(1, std::memory_order_relaxed) + 1 == kJobs)
            qb::Main::stop();
    }
};

// Builds a WorkerPool from its workers and fans kJobs out round-robin via pool.next().
class Dispatcher : public qb::Actor {
    std::vector<qb::ActorId> _workers;

public:
    explicit Dispatcher(std::vector<qb::ActorId> w)
        : _workers(std::move(w)) {}
    qb::io::async::task<bool>
    onInit() override {
        qb::WorkerPool pool{_workers};
        EXPECT_EQ(pool.size(), static_cast<std::size_t>(kWorkers));
        for (int i = 0; i < kJobs; ++i)
            push<Job>(pool.next(), i);
        g_dispatcher_ran.store(true, std::memory_order_relaxed);
        co_return true;
    }
};

// ===========================================================================
// Round-robin dispatch -> exact even distribution.
// ===========================================================================
TEST(RoutingDispatch, RoundRobinDistributesEvenly) {
    reset_counters();
    qb::Main                 main;
    std::vector<qb::ActorId> workers;
    for (int i = 0; i < kWorkers; ++i)
        workers.push_back(main.addActor<Worker>(0, i));
    main.addActor<Dispatcher>(0, workers);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_dispatcher_ran.load()) << "Dispatcher::onInit must have run and fanned the jobs out";
    // pool.next() over 9 jobs / 3 workers -> exactly 3 each, in delivery order.
    EXPECT_EQ(g_worker_recv[0].load(), 3);
    EXPECT_EQ(g_worker_recv[1].load(), 3);
    EXPECT_EQ(g_worker_recv[2].load(), 3);
    EXPECT_EQ(g_jobs_done.load(), kJobs); // no job lost, none duplicated
}

// ===========================================================================
// Dispatch to Activating workers -> every job stashed and replayed.
// ===========================================================================
TEST(RoutingDispatch, DispatchToActivatingWorkersStashesAndReplays) {
    reset_counters();
    qb::Main                 main;
    std::vector<qb::ActorId> workers;
    for (int i = 0; i < kWorkers; ++i)
        workers.push_back(main.addActor<ActivatingWorker>(0, i)); // Activating workers
    main.addActor<Dispatcher>(0, workers);                        // dispatches while they Activate

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_dispatcher_ran.load()) << "Dispatcher::onInit must have run while workers were Activating";
    const int total = g_worker_recv[0].load() + g_worker_recv[1].load() + g_worker_recv[2].load();
    EXPECT_EQ(total, kJobs) << "every dispatched job must be stashed at its Activating worker then replayed";
    EXPECT_EQ(g_jobs_done.load(), kJobs);
    // Round-robin still held through the stash: each worker got exactly its share, nothing dropped.
    EXPECT_EQ(g_worker_recv[0].load(), 3);
    EXPECT_EQ(g_worker_recv[1].load(), 3);
    EXPECT_EQ(g_worker_recv[2].load(), 3);
}
