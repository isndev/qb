/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-patterns.cpp
 * @brief The high-level pattern library run INSIDE `onInit()` ("patterns-in-init").
 *
 * Every `qb::*` request/response and resilience pattern that an actor might use at steady state is
 * also legal during initialization, because the ask-reply gate routes correlated replies to the
 * still-Activating coroutine (see init-ask-gate.cpp) and the scope-bound helpers (`ctx.sleep`, the
 * per-actor scope) exist from the first `onInit` resume. This file is the UNION of the
 * patterns-in-init coverage formerly split across test-actor-init-robustness.cpp and the torture
 * suite's sections B/H, deduplicated so each distinct scenario appears exactly once. It proves the
 * FUNCTIONAL (happy / caught-timeout / compensation) behavior of:
 *
 *   - `qb::ask` that TIMES OUT, caught → the init still activates;
 *   - `qb::ask_retry` against a flaky responder that answers on the 2nd attempt;
 *   - `qb::ask_all` gathering from two distinct-valued peers (sum check, not just count);
 *   - `qb::ask_any` resolving on the first reply (a fast peer beats an Activating one);
 *   - `qb::ask_guarded` through a closed circuit breaker;
 *   - `qb::ask_quorum` (k-of-N);
 *   - `qb::ask_by` (absolute deadline);
 *   - `qb::run_saga` happy path (no compensation) AND failure path (compensation runs exactly once);
 *   - `qb::rate_limiter` (3rd acquire waits for a refill, all during init);
 *   - `qb::bulkhead` (hold a slot across an in-init ask);
 *   - `qb::batcher` (count-triggered flush during init);
 *   - `qb::ask_stream` (chunks delivered to the Activating asker via the continuation registry);
 *   - `qb::require` (discover-before-activate).
 *
 * Deadline-driven CANCELLATION of these patterns lives in init-deadline.cpp; the bare ask-reply gate
 * proof lives in init-ask-gate.cpp. tier=system. Every value assertion checks a framework-computed
 * result (e.g. `key * mult`), and each in-actor effect is mirrored to a post-`join()` atomic.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/core/patterns.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::Cfg;
using qb::test::CfgService;

namespace {

// A configurable peer that answers `key * _mult` (distinct mults give distinct, verifiable sums).
class ConfigPeer : public qb::Actor {
    int _mult;

public:
    explicit ConfigPeer(int m)
        : _mult(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &q) {
        const int m = _mult;
        qb::answer(*this, q, [m](Cfg const &r) { return r.key * m; });
    }
};

// Never answers → askers time out against it.
class SilentPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &) {}
};

// ===========================================================================
// 1. ask that TIMES OUT (caught) — the init still completes.
// ===========================================================================
std::atomic<bool> g_to_timed_out{false};
std::atomic<bool> g_to_activated{false};

class AsksSilentInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksSilentInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        try {
            (void) co_await qb::ask(context(), _peer, Cfg{1}, 30ms);
        } catch (const qb::io::async::timeout_error &) {
            g_to_timed_out.store(true);
        }
        g_to_activated.store(true);
        qb::Main::stop(); // tear the engine down cleanly
        co_return true;   // a caught in-init timeout still activates the actor
    }
};

TEST(InitPatterns, InInitAskTimeoutIsCaughtAndActivates) {
    g_to_timed_out.store(false);
    g_to_activated.store(false);
    qb::Main   main;
    const auto peer = main.addActor<SilentPeer>(0);
    main.addActor<AsksSilentInInit>(0, peer);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_to_timed_out.load()) << "the in-init ask must throw timeout_error";
    EXPECT_TRUE(g_to_activated.load()) << "a caught timeout must still activate the actor";
}

// ===========================================================================
// 2. ask_retry against a flaky responder that answers on the 2nd attempt.
// ===========================================================================
class FlakyResponder : public qb::Actor {
    int _count{0};
    int _reply_on;

public:
    explicit FlakyResponder(int reply_on)
        : _reply_on(reply_on) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &q) {
        if (++_count >= _reply_on)
            qb::answer(*this, q, [](Cfg const &r) { return r.key * 10; });
    }
};

std::atomic<int> g_retry_value{-1};

class AskRetryInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskRetryInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        qb::retry_policy pol;
        pol.max_attempts = 5;
        pol.backoff      = 10ms;
        pol.max_backoff  = 40ms;
        auto r           = co_await qb::ask_retry(context(), _peer, Cfg{4}, 25ms, pol); // retries during init
        g_retry_value.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, AskRetryInsideOnInitSucceedsAfterRetry) {
    g_retry_value.store(-1);
    qb::Main   main;
    const auto flaky = main.addActor<FlakyResponder>(0, 2); // answers on the 2nd attempt
    main.addActor<AskRetryInInit>(0, flaky);
    main.start(false);
    main.join();
    EXPECT_EQ(g_retry_value.load(), 40); // 4 * 10, after a retry
}

// ===========================================================================
// 3. ask_all gathers from two distinct-valued peers (sum, not just count).
// ===========================================================================
std::atomic<int> g_askall_sum{-1};

class AskAllInInit : public qb::Actor {
    qb::ActorId _a, _b;

public:
    AskAllInInit(qb::ActorId a, qb::ActorId b)
        : _a(a)
        , _b(b) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        std::vector<qb::ActorId> peers{_a, _b};
        auto                     replies = co_await qb::ask_all(context(), peers, Cfg{3}, 500ms);
        int                      sum     = 0;
        for (auto const &r : replies)
            sum += r.response;
        g_askall_sum.store(sum);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, AskAllInsideOnInitGathers) {
    g_askall_sum.store(-1);
    qb::Main   main;
    const auto a = main.addActor<ConfigPeer>(0, 10);  // 3*10 = 30
    const auto b = main.addActor<ConfigPeer>(0, 100); // 3*100 = 300
    main.addActor<AskAllInInit>(0, a, b);
    main.start(false);
    main.join();
    EXPECT_EQ(g_askall_sum.load(), 330); // distinct values prove both peers were gathered
}

// ===========================================================================
// 4. ask_any resolves on the FIRST reply — a fast peer beats an Activating one.
// ===========================================================================
// The SLOW peer. Its multiplier is deliberately NOT the 10 that `CfgService` (the fast peer) uses:
// with both answering `key * 10` the gathered value is 60 whichever one replied first, so the
// assertion below could not tell the winner from the loser and held for either outcome. 7 makes the
// two answers distinguishable (42 vs 60), which is the only thing that makes this case falsifiable.
class AsyncResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_await context().sleep(25ms); // Activating — the request stashes here, replies late
        co_return true;
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key * 7; });
        kill();
    }
};

std::atomic<int> g_any_value{-1};

class AskAnyInInit : public qb::Actor {
    std::vector<qb::ActorId> _targets;

public:
    explicit AskAnyInInit(std::vector<qb::ActorId> t)
        : _targets(std::move(t)) {}
    qb::io::async::task<bool>
    onInit() override {
        auto reply = co_await qb::ask_any(context(), _targets, Cfg{6}, 2s);
        g_any_value.store(reply.response);
        kill();
        co_return true;
    }
};

TEST(InitPatterns, AskAnyToActivatingTargetResolvesOnFastReply) {
    g_any_value.store(-1);
    qb::Main   main;
    const auto slow = main.addActor<AsyncResponder>(0); // Activating target (x7, replies late)
    const auto fast = main.addActor<CfgService>(0);     // already-active target (x10) — wins
    main.addActor<AskAnyInInit>(0, std::vector<qb::ActorId>{slow, fast});
    main.start(false);
    main.join();
    // 6*10 from the fast peer. The slow peer would answer 42, so this equality is what identifies
    // the winner — the race outcome, not merely that SOME peer replied.
    EXPECT_EQ(g_any_value.load(), 60) << "ask_any must resolve on the already-active peer's reply, not the Activating one's";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 5. ask_guarded through a closed circuit breaker.
// ===========================================================================
std::atomic<int> g_guarded_value{-1};

class AskGuardedInInit : public qb::Actor {
    qb::ActorId                         _svc;
    std::shared_ptr<qb::CircuitBreaker> _breaker = std::make_shared<qb::CircuitBreaker>(3u, 100ms);

public:
    explicit AskGuardedInInit(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        auto reply = co_await qb::ask_guarded(context(), _breaker, _svc, Cfg{8}, 1s);
        g_guarded_value.store(reply.response);
        kill();
        co_return true;
    }
};

TEST(InitPatterns, AskGuardedAcrossActivationBoundary) {
    g_guarded_value.store(-1);
    qb::Main   main;
    const auto svc = main.addActor<CfgService>(0);
    main.addActor<AskGuardedInInit>(0, svc);
    main.start(false);
    main.join();
    EXPECT_EQ(g_guarded_value.load(), 80); // breaker closed → guarded ask succeeds during Activating
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 6. ask_quorum (k-of-N) — the first 2 of 3 reached while Activating.
// ===========================================================================
std::atomic<int> g_quorum_count{-1};

class AskQuorumInInit : public qb::Actor {
    std::vector<qb::ActorId> _peers;

public:
    explicit AskQuorumInInit(std::vector<qb::ActorId> peers)
        : _peers(std::move(peers)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        auto got = co_await qb::ask_quorum(context(), _peers, 2, Cfg{3}, 1s);
        g_quorum_count.store(static_cast<int>(got.size()));
        qb::Main::stop();
        co_return true;
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, AskQuorumInsideOnInit) {
    g_quorum_count.store(-1);
    qb::Main                 main;
    std::vector<qb::ActorId> peers;
    peers.push_back(main.addActor<ConfigPeer>(0, 10));
    peers.push_back(main.addActor<ConfigPeer>(0, 100));
    peers.push_back(main.addActor<ConfigPeer>(0, 1000));
    main.addActor<AskQuorumInInit>(0, peers);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_quorum_count.load(), 2); // first 2 of 3 reached during Activating
}

// ===========================================================================
// 7. ask_by (absolute deadline).
// ===========================================================================
std::atomic<int> g_askby_value{-1};

class AskByInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskByInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        const auto dl = qb::deadline_in(context(), 1s);
        auto       r  = co_await qb::ask_by(context(), _peer, Cfg{5}, dl);
        g_askby_value.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, AskByDeadlineInsideOnInit) {
    g_askby_value.store(-1);
    qb::Main   main;
    const auto p = main.addActor<ConfigPeer>(0, 10); // 5 * 10 = 50
    main.addActor<AskByInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_askby_value.load(), 50);
}

// ===========================================================================
// 8. run_saga HAPPY path inside onInit — both steps succeed, no compensation.
// ===========================================================================
std::atomic<int> g_saga_steps{0};

class RunsSagaInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await qb::run_saga(context(), [](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
            g_saga_steps.fetch_add(1);
            saga.on_compensate([]() -> qb::io::async::task<void> { co_return; });
            co_await ctx.sleep(5ms);
            g_saga_steps.fetch_add(1);
            co_return; // both steps succeed → no compensation
        });
        kill();
        co_return true;
    }
};

TEST(InitPatterns, RunSagaHappyPathInsideOnInit) {
    g_saga_steps.store(0);
    qb::Main main;
    main.addActor<RunsSagaInInit>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_saga_steps.load(), 2); // both steps ran, no rollback
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 9. run_saga FAILURE path inside onInit — a step times out → compensation runs
//    exactly once (itself an in-init ask) → the init fails.
// ===========================================================================
std::atomic<bool> g_saga_compensated{false};
std::atomic<int>  g_saga_compensate_calls{0};
std::atomic<bool> g_saga_init_failed{false};

// Always answers (used for the saga's step-1 reservation + its compensation undo).
class Reserver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &q) {
        qb::answer(*this, q, [](Cfg const &r) { return r.key; });
    }
};

class SagaInInit : public qb::Actor {
    qb::ActorId _reserver;
    qb::ActorId _silent;

public:
    SagaInInit(qb::ActorId reserver, qb::ActorId silent)
        : _reserver(reserver)
        , _silent(silent) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        auto reserver = _reserver;
        auto silent   = _silent;
        try {
            co_await qb::run_saga(context(), [reserver, silent](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                (void) co_await qb::ask(ctx, reserver, Cfg{1}, 500ms); // step 1 succeeds
                saga.on_compensate([ctx, reserver]() -> qb::io::async::task<void> {
                    (void) co_await qb::ask(ctx, reserver, Cfg{2}, 500ms); // undo (in-init ask)
                    g_saga_compensate_calls.fetch_add(1);
                    g_saga_compensated.store(true);
                });
                (void) co_await qb::ask(ctx, silent, Cfg{3}, 30ms); // step 2 TIMES OUT → rollback
            });
        } catch (const qb::io::async::timeout_error &) {
            g_saga_init_failed.store(true);
            qb::Main::stop();
            co_return false; // the saga failed → fail the init
        }
        qb::Main::stop();
        co_return true; // not reached
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, SagaInsideOnInitCompensatesThenFailsInit) {
    g_saga_compensated.store(false);
    g_saga_compensate_calls.store(0);
    g_saga_init_failed.store(false);
    qb::Main   main;
    const auto reserver = main.addActor<Reserver>(0);
    const auto silent   = main.addActor<SilentPeer>(0);
    main.addActor<SagaInInit>(0, reserver, silent);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_saga_init_failed.load());       // step 2 timeout failed the saga + the init
    EXPECT_TRUE(g_saga_compensated.load());       // compensation (an in-init ask) ran
    EXPECT_EQ(g_saga_compensate_calls.load(), 1); // exactly once (no double compensation)
}

// ===========================================================================
// 10. rate_limiter — 3rd acquire waits for a refill, all during onInit.
// ===========================================================================
std::atomic<int> g_rl{-1};

class RateLimiterInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto rl = std::make_shared<qb::rate_limiter>(2.0, 30ms); // 2 tokens, +1 per 30ms
        co_await rl->acquire(context());                         // token 1 (immediate)
        co_await rl->acquire(context());                         // token 2 (immediate)
        co_await rl->acquire(context());                         // waits ~30ms (refill) — scope-bound sleep
        g_rl.store(3);
        qb::Main::stop();
        co_return true;
    }
};

TEST(InitPatterns, RateLimiterInsideOnInit) {
    g_rl.store(-1);
    qb::Main main;
    main.addActor<RateLimiterInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_rl.load(), 3); // all three acquired during init (3rd after a refill wait)
}

// ===========================================================================
// 11. bulkhead — hold a slot across an in-init ask.
// ===========================================================================
std::atomic<int> g_bh{-1};

class BulkheadInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit BulkheadInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        auto bh   = std::make_shared<qb::bulkhead>(1);
        auto slot = co_await bh->enter(context()); // admitted immediately (free)
        auto r    = co_await qb::ask(context(), _peer, Cfg{7}, 1s);
        g_bh.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Cfg &e) {
        resolve_ask(e);
    }
};

TEST(InitPatterns, BulkheadInsideOnInit) {
    g_bh.store(-1);
    qb::Main   main;
    const auto p = main.addActor<ConfigPeer>(0, 10); // 7 * 10 = 70
    main.addActor<BulkheadInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bh.load(), 70);
}

// ===========================================================================
// 12. batcher — count-triggered flush during init.
// ===========================================================================
std::atomic<int> g_batch{-1};

class BatcherInInit : public qb::Actor {
    qb::batcher<int> _batch{3, 5s, [](std::vector<int> &&b) { g_batch.store(static_cast<int>(b.size())); }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1);
        _batch.add(context(), 2);
        _batch.add(context(), 3); // count == max → synchronous flush during init
        qb::Main::stop();
        co_return true;
    }
};

TEST(InitPatterns, BatcherInsideOnInit) {
    g_batch.store(-1);
    qb::Main main;
    main.addActor<BatcherInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch.load(), 3); // flushed by count during init
}

// ===========================================================================
// 13. ask_stream — chunks reach the Activating asker through the continuation registry.
// ===========================================================================
struct SFeed : public qb::StreamRequest<int> {
    int count{0};
    SFeed() = default;
    explicit SFeed(int c)
        : count(c) {}
};

class StreamProd : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SFeed>(*this);
        co_return true;
    }
    void
    on(SFeed &e) {
        for (int i = 0; i < e.count; ++i)
            qb::yield_answer(*this, e, i + 1);
        qb::end_stream(*this, e);
    }
};

std::atomic<int> g_stream_n{-1};

class AskStreamInInit : public qb::Actor {
    qb::ActorId _prod;

public:
    explicit AskStreamInInit(qb::ActorId p)
        : _prod(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SFeed>(*this);
        auto s = qb::ask_stream(context(), _prod, SFeed{3}, 1s); // consumed DURING onInit
        int  n = 0;
        while (auto c = co_await s.next())
            ++n;
        g_stream_n.store(n);
        qb::Main::stop();
        co_return true;
    }
    void
    on(SFeed &e) {
        (void) resolve_ask(e); // chunks are AskEvents → continuation registry
    }
};

TEST(InitPatterns, AskStreamInsideOnInitDeliversChunks) {
    g_stream_n.store(-1);
    qb::Main   main;
    const auto p = main.addActor<StreamProd>(0);
    main.addActor<AskStreamInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_stream_n.load(), 3); // all chunks delivered while the asker was Activating
}

// ===========================================================================
// 14. require — discover-before-activate (replies via the continuation registry).
// ===========================================================================
class InitWorker : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

std::atomic<int> g_require_count{-1};

class RequireInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // No on(RequireEvent) handler — Actor routes discovery replies by default.
        auto found = co_await qb::require<InitWorker>(context(), 150ms); // DIRECT co_await in onInit
        g_require_count.store(static_cast<int>(found.size()));
        qb::Main::stop();
        co_return true;
    }
};

TEST(InitPatterns, RequireInsideOnInitDiscoversAll) {
    g_require_count.store(-1);
    qb::Main main;
    main.addActor<InitWorker>(0);
    main.addActor<InitWorker>(0);
    main.addActor<RequireInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_require_count.load(), 2); // both workers discovered DURING onInit
}

} // namespace
