/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/concurrency/actor-message-serialization.cpp
 * @brief Concurrent senders feed a single counter actor; the engine SERIALIZES every increment so
 *        not one is lost — proven by an EXACT total (no `>= 0.9 * N` slack that would hide a 10% drop).
 *
 * `NUM_WORKERS` workers each emit exactly `OPS_PER_WORKER` unit increments at a single `CounterActor`.
 * Per-VirtualCore single-thread semantics mean the counter's handler runs serially even though the
 * senders run "concurrently", so the arithmetic is deterministic:
 *   - the counter's running `_total` must equal `NUM_WORKERS * OPS_PER_WORKER`;
 *   - the SUM of the per-bucket counters must equal that same emitted total (no increment lost or
 *     double-applied);
 *   - the count is read via a Query mailbox-ordered AFTER every increment, so completion is
 *     event-driven — no `2s` safety timeout, no `500us` inter-op callback chain, no slack.
 *
 * The original asserted only `ops >= NUM_OPERATIONS * 0.9` (a 10% loss passed) and carried a dead
 * `DummyActor` that did nothing but hold a core open. Both are gone.
 */

#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <vector>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace {
constexpr int NUM_BUCKETS    = 10;
constexpr int NUM_WORKERS    = 5;
constexpr int OPS_PER_WORKER = 200;
constexpr int TOTAL_OPS      = NUM_WORKERS * OPS_PER_WORKER; // 1000

// Read after join() (happens-before via Main::join()).
std::atomic<int>  g_emitted{0};       // increments actually emitted by the workers
std::atomic<int>  g_counter_total{0}; // the counter's own running total at query time
std::atomic<int>  g_bucket_sum{0};    // sum of the per-bucket counters at query time
std::atomic<bool> g_done{false};
} // namespace

// Protocol.
struct Increment : public qb::Event {
    int bucket;
    explicit Increment(int b)
        : bucket(b) {}
};
struct WorkerDone : public qb::Event {};
struct QueryTotal : public qb::Event {
    qb::ActorId reply_to;
    explicit QueryTotal(qb::ActorId r)
        : reply_to(r) {}
};
struct TotalReport : public qb::Event {
    int total;
    int bucket_sum;
    TotalReport(int t, int s)
        : total(t)
        , bucket_sum(s) {}
};

// The single serialization point: every Increment lands here and is applied one at a time.
class CounterActor : public qb::Actor {
    std::array<int, NUM_BUCKETS> _buckets{};
    int                          _total = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Increment>(*this);
        registerEvent<QueryTotal>(*this);
        co_return true;
    }

    void
    on(const Increment &e) {
        ++_buckets[e.bucket]; // deterministic: a fixed bucket per worker, applied serially
        ++_total;
    }

    void
    on(const QueryTotal &e) {
        int sum = 0;
        for (int v : _buckets)
            sum += v;
        to(e.reply_to).push<TotalReport>(_total, sum);
    }
};

// Emits exactly OPS_PER_WORKER unit increments at a fixed bucket, then notifies the coordinator.
// All pushes go out in onInit (mailbox-ordered, no timers); the coordinator's later QueryTotal is
// ordered behind every increment on the counter's mailbox, so it observes the final total.
class Worker : public qb::Actor {
    qb::ActorId _counter;
    qb::ActorId _coord;
    int         _bucket;

public:
    Worker(qb::ActorId counter, qb::ActorId coord, int bucket)
        : _counter(counter)
        , _coord(coord)
        , _bucket(bucket) {}

    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < OPS_PER_WORKER; ++i) {
            to(_counter).push<Increment>(_bucket);
            g_emitted.fetch_add(1);
        }
        to(_coord).push<WorkerDone>();
        kill();
        co_return true;
    }
};

// Spawns the counter + workers; once every worker has reported done, queries the exact total. The
// query is pushed to the counter (same core) AFTER all WorkerDone are seen, hence after every
// Increment in the counter's mailbox — so the reported total is final.
class SerializationCoordinator : public qb::Actor {
    qb::ActorId _counter;
    int         _done = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WorkerDone>(*this);
        registerEvent<TotalReport>(*this);

        _counter = addRefActor<CounterActor>().id();
        for (int w = 0; w < NUM_WORKERS; ++w)
            addRefActor<Worker>(_counter, id(), /*bucket*/ w % NUM_BUCKETS);
        co_return true;
    }

    void
    on(const WorkerDone &) {
        if (++_done == NUM_WORKERS)
            to(_counter).push<QueryTotal>(id());
    }

    void
    on(const TotalReport &e) {
        g_counter_total.store(e.total);
        g_bucket_sum.store(e.bucket_sum);
        g_done.store(true);
        qb::Main::stop();
        kill();
    }
};

TEST(MessageSerialization, EveryConcurrentIncrementIsAppliedExactlyOnce) {
    g_emitted.store(0);
    g_counter_total.store(0);
    g_bucket_sum.store(0);
    g_done.store(false);

    qb::Main main;
    main.core(0).addActor<SerializationCoordinator>();

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_done.load()) << "the coordinator must have received the final report";

    // Every emitted increment was applied — EXACTLY, no slack: under-delivery (loss) and
    // over-delivery (double-apply) both fail here.
    EXPECT_EQ(g_emitted.load(), TOTAL_OPS) << "every worker emitted its full quota";
    EXPECT_EQ(g_counter_total.load(), TOTAL_OPS) << "the counter applied every increment exactly once";
    // The per-bucket sum must reconcile with the running total — no increment routed to a phantom
    // bucket or dropped.
    EXPECT_EQ(g_bucket_sum.load(), g_counter_total.load()) << "bucket sum must equal the running total";
    EXPECT_EQ(g_bucket_sum.load(), TOTAL_OPS);
}
