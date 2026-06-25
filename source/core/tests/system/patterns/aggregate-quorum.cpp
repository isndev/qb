/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/aggregate-quorum.cpp
 * @brief Quorum scatter-gather: `qb::ask_quorum(ctx, targets, k, req, timeout)` — first K of N.
 *
 * `ask_quorum` fills the gap between `ask_any` (k = 1) and `ask_all` (k = N): it asks every target
 * and resolves with the FIRST `k` successful replies, in completion order. It throws `timeout_error`
 * (carrying the first underlying error) once the quorum becomes provably UNREACHABLE, and
 * `cancelled_error` if the asker is killed. Proven here against the running engine:
 *   - HAPPY PATH: 3-of-5 returns exactly k results whose sum is the responder-computed value;
 *   - UNREACHABLE: 2 repliers + 3 silent → quorum of 3 impossible → timeout_error, no result;
 *   - CANCEL: killed while parked on silent targets → cancelled_error (not timeout);
 *   - EDGES: k == 0 resolves immediately with an empty vector (no wait); k > n clamps to n;
 *   - FRAME RECLAIM: a completed quorum leaves the coroutine-frame allocator balanced (the awaiter
 *     plus all N collectors are reclaimed) — caught via the thread-local live_frames invariant.
 *
 * Outcomes are mirrored to atomics (size / sum / which exception) so a never-scheduled coroutine
 * cannot pass vacuously: a successful run asserts an exact size, the failure runs assert the exact
 * exception flag. Shutdown is event-driven (the asker stops the engine when ask_quorum resolves) or,
 * for the cancel test, the moment cancellation is observed.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <vector>
#include "../../shared/ProbeResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::FastResponder;
using qb::test::Probe;
using qb::test::SilentResponder;

namespace {
std::atomic<int>  g_q_size{-1};
std::atomic<int>  g_q_sum{0};
std::atomic<bool> g_q_timeout{false};
std::atomic<bool> g_q_cancelled{false};
std::atomic<bool> g_q_ran{false}; // the asker coroutine reached a terminal state (ran-guard)

void
reset_quorum() {
    g_q_size.store(-1);
    g_q_sum.store(0);
    g_q_timeout.store(false);
    g_q_cancelled.store(false);
    g_q_ran.store(false);
}
} // namespace

class QuorumAsker : public qb::Actor {
    std::vector<qb::ActorId> _targets;
    std::size_t              _k;
    qb::duration             _timeout;
    bool                     _stop_on_done;

public:
    QuorumAsker(std::vector<qb::ActorId> targets, std::size_t k, qb::duration timeout, bool stop_on_done)
        : _targets(std::move(targets))
        , _k(k)
        , _timeout(timeout)
        , _stop_on_done(stop_on_done) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        auto targets = _targets;
        auto k       = _k;
        auto to      = _timeout;
        auto stop    = _stop_on_done;
        spawn([targets, k, to, stop](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                auto r = co_await qb::ask_quorum(c, targets, k, Probe{10}, to);
                g_q_size.store(static_cast<int>(r.size()));
                int s = 0;
                for (auto const &e : r)
                    s += e.response;
                g_q_sum.store(s);
            } catch (const qb::io::async::timeout_error &) {
                g_q_timeout.store(true);
            } catch (const qb::io::async::cancelled_error &) {
                g_q_cancelled.store(true);
            }
            g_q_ran.store(true);
            if (stop)
                qb::Main::stop(); // event-driven shutdown for the self-resolving cases
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

TEST(AskQuorum, ReturnsFirstKResults) {
    reset_quorum();
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < 5; ++i)
        targets.push_back(main.addActor<FastResponder>(0));
    main.addActor<QuorumAsker>(0, targets, /*k*/ 3, 500ms, /*stop*/ true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_ran.load()) << "the quorum asker coroutine must have run";
    EXPECT_EQ(g_q_size.load(), 3) << "exactly k results, no more";
    EXPECT_EQ(g_q_sum.load(), 3 * 11) << "each gathered response = seq(10)+1 = 11";
    EXPECT_FALSE(g_q_timeout.load());
    EXPECT_FALSE(g_q_cancelled.load());
}

TEST(AskQuorum, UnreachableThrowsTimeout) {
    reset_quorum();
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    targets.push_back(main.addActor<FastResponder>(0)); // 2 can answer …
    targets.push_back(main.addActor<FastResponder>(0));
    targets.push_back(main.addActor<SilentResponder>(0)); // … 3 never do → quorum of 3 impossible
    targets.push_back(main.addActor<SilentResponder>(0));
    targets.push_back(main.addActor<SilentResponder>(0));
    main.addActor<QuorumAsker>(0, targets, /*k*/ 3, 40ms, /*stop*/ true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_ran.load());
    EXPECT_TRUE(g_q_timeout.load()) << "3 failures > N-k (2) → unreachable → timeout_error";
    EXPECT_EQ(g_q_size.load(), -1) << "no result delivered on an unreachable quorum";
}

// Kills the asker while it is parked on silent targets, then arms a generous backstop stop so a
// regression of the cancel path fails loudly via the assertions rather than hanging.
class QuorumKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit QuorumKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill while parked
        qb::io::async::callback([] { qb::Main::stop(); }, 2s);                 // backstop only
        co_return true;
    }
};

TEST(AskQuorum, CancelledOnKill) {
    reset_quorum();
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < 3; ++i)
        targets.push_back(main.addActor<SilentResponder>(0)); // never answer (long timeout) → parked
    // stop=false: the asker must NOT stop the engine; cancellation observation drives shutdown.
    const auto asker = main.addActor<QuorumAsker>(0, targets, /*k*/ 2, 5s, /*stop*/ false);
    main.addActor<QuorumKiller>(0, asker);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_ran.load());
    EXPECT_TRUE(g_q_cancelled.load()) << "killed while waiting → cancelled_error";
    EXPECT_FALSE(g_q_timeout.load());
}

TEST(AskQuorum, ZeroKReturnsEmptyImmediately) {
    reset_quorum();
    qb::Main   main;
    const auto a = main.addActor<FastResponder>(0);
    const auto b = main.addActor<FastResponder>(0);
    main.addActor<QuorumAsker>(0, std::vector<qb::ActorId>{a, b}, /*k=*/0, 1s, /*stop*/ true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_ran.load());
    EXPECT_EQ(g_q_size.load(), 0) << "k == 0 resolves immediately with nothing";
    EXPECT_FALSE(g_q_timeout.load()) << "…and does NOT wait out the window";
}

TEST(AskQuorum, KClampedToN) {
    reset_quorum();
    qb::Main   main;
    const auto a = main.addActor<FastResponder>(0);
    const auto b = main.addActor<FastResponder>(0);
    main.addActor<QuorumAsker>(0, std::vector<qb::ActorId>{a, b}, /*k=*/5, 1s, /*stop*/ true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_ran.load());
    EXPECT_EQ(g_q_size.load(), 2) << "k > n clamps to n → both replies, no hang";
    EXPECT_EQ(g_q_sum.load(), 2 * 11);
    EXPECT_FALSE(g_q_timeout.load());
}

TEST(AskQuorum, ReclaimsAllCoroutineFrames) {
    reset_quorum();
    // Single core → this thread is the worker; live_frames is ours to measure across the run.
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main                 main;
        std::vector<qb::ActorId> targets;
        for (int i = 0; i < 5; ++i)
            targets.push_back(main.addActor<FastResponder>(0));
        main.addActor<QuorumAsker>(0, targets, /*k*/ 3, 500ms, /*stop*/ true);
        main.start(false);
        main.join();
    }
    EXPECT_TRUE(g_q_ran.load());
    EXPECT_EQ(g_q_size.load(), 3);
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "ask_quorum leaked coroutine frames (quorum awaiter + N collectors)";
}
