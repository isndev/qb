/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/aggregate-scatter.cpp
 * @brief Bounded scatter-gather: `qb::ask_all(ctx, targets, req, timeout, max_in_flight)`.
 *
 * `ask_all` fans a request out to N targets and awaits every reply; the bounded overload caps how
 * many asks are outstanding at once via a shared cancellation-aware semaphore (a true sliding
 * window — a new ask starts the instant one finishes). Proven here against the running engine, with
 * two independent oracles per run:
 *   1. CONCURRENCY: each responder holds its reply briefly so concurrently-outstanding asks are
 *      observable; an atomic max-in-flight counter is the framework truth — the bounded run never
 *      exceeds the cap, the unbounded run fans out to ALL targets at once;
 *   2. CORRECTNESS: the gathered sum equals N * responder-computed value (each response = seq+1),
 *      so no reply is dropped, duplicated, or mis-correlated regardless of the cap.
 *
 * De-flaked vs the monolith: the unbounded contrast is pinned to the EXACT max-in-flight (== N),
 * not a loose `> CAP`. Shutdown is driven by the asker the instant ask_all resolves. The in-actor
 * gather is mirrored to an atomic guarded by a "ran" flag asserted after join().
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
#include <cstdint>
#include <vector>
#include "../../shared/ProbeResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::Probe;

namespace {
std::atomic<int>  g_inflight{0};     // asks received but not yet answered
std::atomic<int>  g_max_inflight{0}; // peak concurrently-outstanding asks
std::atomic<int>  g_sum{0};          // gathered sum of responses
std::atomic<bool> g_gather_done{false};

void
bump_max() {
    int cur  = g_inflight.load();
    int prev = g_max_inflight.load();
    while (cur > prev && !g_max_inflight.compare_exchange_weak(prev, cur)) { /* retry */
    }
}

void
reset_scatter() {
    g_inflight.store(0);
    g_max_inflight.store(0);
    g_sum.store(0);
    g_gather_done.store(false);
}
} // namespace

// Holds each request briefly before replying, so concurrently-outstanding asks are observable.
class SlowResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe &p) {
        const auto          src  = p.getSource();
        const std::uint64_t corr = p.correlation_id;
        const int           v    = p.seq + 1;
        g_inflight.fetch_add(1);
        bump_max();
        spawn([src, corr, v](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            co_await c.sleep(15ms);
            g_inflight.fetch_sub(1);
            c.template push_to<Probe>(src, 0, corr, v); // reply carries the correlation id + response
        });
    }
};

class BoundedAsker : public qb::Actor {
    std::vector<qb::ActorId> _targets;
    std::size_t              _cap;

public:
    BoundedAsker(std::vector<qb::ActorId> targets, std::size_t cap)
        : _targets(std::move(targets))
        , _cap(cap) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        auto targets = _targets;
        auto cap     = _cap;
        spawn([targets, cap](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            std::vector<Probe> r;
            if (cap == 0)
                r = co_await qb::ask_all(c, targets, Probe{10}, 2s); // unbounded overload
            else
                r = co_await qb::ask_all(c, targets, Probe{10}, 2s, cap);
            int s = 0;
            for (auto const &e : r)
                s += e.response;
            g_sum.store(s);
            g_gather_done.store(true);
            qb::Main::stop(); // event-driven: stop the instant ask_all resolves
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

TEST(AskAllBounded, CapLimitsConcurrencyAndKeepsAllResults) {
    reset_scatter();
    constexpr int            N   = 6;
    constexpr int            CAP = 2;
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < N; ++i)
        targets.push_back(main.addActor<SlowResponder>(0));
    main.addActor<BoundedAsker>(0, targets, CAP);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_gather_done.load()) << "the ask_all coroutine must have resolved";
    EXPECT_LE(g_max_inflight.load(), CAP) << "never more than CAP asks outstanding — the cap holds";
    EXPECT_GE(g_max_inflight.load(), 1) << "at least one ask was actually outstanding (teeth)";
    EXPECT_EQ(g_sum.load(), N * 11) << "every response gathered exactly once (seq 10 + 1 = 11)";
}

TEST(AskAllBounded, UnboundedFansOutToAllTargetsAtOnce) {
    reset_scatter();
    constexpr int            N = 6;
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < N; ++i)
        targets.push_back(main.addActor<SlowResponder>(0));
    main.addActor<BoundedAsker>(0, targets, 0); // 0 => unbounded
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_gather_done.load());
    // Unbounded keeps all N asks in flight at once (each responder holds 15ms) — exact contrast with
    // the bounded cap, not a loose `> CAP`.
    EXPECT_EQ(g_max_inflight.load(), N) << "unbounded ask_all must fan out to every target at once";
    EXPECT_EQ(g_sum.load(), N * 11);
}
