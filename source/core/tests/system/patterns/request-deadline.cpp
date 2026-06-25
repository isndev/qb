/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/request-deadline.cpp
 * @brief Absolute-deadline budget propagation: `qb::deadline_in` / `qb::remaining` / `qb::ask_by`.
 *
 * A `qb::deadline` is an ABSOLUTE completion time threaded through a chain of `ask_by` calls so the
 * WHOLE chain is bounded end-to-end (unlike a relative per-`ask` timeout that resets at each hop).
 * Proven here against the running engine, with three exact oracles in one coroutine pass:
 *   - BUDGET: `remaining(deadline_in(ctx, 100ms), ctx)` reads back EXACTLY 100ms — the context's
 *     `time()` is cached per loop pass, so building and querying the deadline in the same pass is
 *     exact to the nanosecond (`100'000'000 ns`), not an approximate band;
 *   - FAIL-FAST: `ask_by` with an already-spent deadline (`deadline{0}`) throws `timeout_error`
 *     IMMEDIATELY without sending a request;
 *   - SUCCESS: `ask_by` with a future deadline completes within budget and returns the
 *     responder-computed value (seq 5 + 1 = 6).
 *
 * The three outcomes are mirrored to atomics behind a single "ran" flag asserted after join(), so a
 * never-scheduled coroutine cannot pass any of them vacuously. Shutdown is driven by the asker the
 * instant its coroutine finishes — no wall-clock stop offset.
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
#include "../../shared/ProbeResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::FastResponder;
using qb::test::Probe;

namespace {
std::atomic<long> g_dl_remaining_ns{-1};
std::atomic<bool> g_dl_past_timeout{false};
std::atomic<int>  g_dl_val{-1};
std::atomic<bool> g_dl_ran{false}; // the asker coroutine reached the end (ran-guard)
} // namespace

class DeadlineAsker : public qb::Actor {
    qb::ActorId _fast;

public:
    explicit DeadlineAsker(qb::ActorId fast)
        : _fast(fast) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        auto fast = _fast;
        spawn([fast](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            // `time()` is cached per loop pass, so deadline_in + remaining in the same pass is exact.
            const auto dl = qb::deadline_in(c, 100ms);
            g_dl_remaining_ns.store(static_cast<long>(qb::remaining(dl, c).count()));

            // Past deadline → fail fast (no request sent).
            try {
                (void) co_await qb::ask_by(c, fast, Probe{5}, qb::deadline{0});
            } catch (const qb::io::async::timeout_error &) {
                g_dl_past_timeout.store(true);
            }
            // Future deadline → succeeds within budget.
            try {
                auto r = co_await qb::ask_by(c, fast, Probe{5}, qb::deadline_in(c, 500ms));
                g_dl_val.store(r.response);
            } catch (...) {
            }
            g_dl_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant the coroutine finishes
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

TEST(Deadline, BudgetPropagationAndFailFast) {
    g_dl_remaining_ns.store(-1);
    g_dl_past_timeout.store(false);
    g_dl_val.store(-1);
    g_dl_ran.store(false);
    qb::Main   main;
    const auto fast = main.addActor<FastResponder>(0);
    main.addActor<DeadlineAsker>(0, fast);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_dl_ran.load()) << "the deadline asker coroutine must have run";
    EXPECT_EQ(g_dl_remaining_ns.load(), 100'000'000L) << "exact within one cached-time loop pass";
    EXPECT_TRUE(g_dl_past_timeout.load()) << "an already-spent budget must fail fast";
    EXPECT_EQ(g_dl_val.load(), 6) << "future deadline → reply (seq 5 + 1)";
}
