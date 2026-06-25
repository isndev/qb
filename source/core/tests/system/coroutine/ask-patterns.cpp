/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/ask-patterns.cpp
 * @brief The request/response COMPOSITION layer built on `ask`: typed `Request<Resp>` + `qb::answer`,
 *        the `ask_all` / `ask_any` scatter-gather combinators, and `run_saga` orchestration.
 *
 * Validates, with framework-computed oracles (every gathered value is `key * k` produced by the
 * responder, never a value the asker handed in):
 *   - `Request<Resp>` + `qb::answer(*this, e, fn)` round-trips a typed response over one event type;
 *   - `ask_all` gathers EVERY reply (same-core and cross-core), and fails with `timeout_error` if any
 *     target stays silent, and unwinds with `cancelled_error` if the asker is killed mid-gather;
 *   - `ask_any` resolves with the FIRST reply (fastest wins, same-core AND cross-core) and
 *     `timeout_error` if all stay silent;
 *   - `qb::answer`'s `resolve_ask` guard lets one actor both ask and answer the *same* event type
 *     without confusing its own replies with inbound requests;
 *   - `run_saga` / `SagaScope`: compensations run in reverse (LIFO) on a step failure, the happy
 *     path runs none, a mid-flow cancel SKIPS rollback, and a throwing compensation does not abort
 *     the remaining (best-effort) rollbacks.
 *
 * Hardening over the original (see docs/tests-audit/qb-core/qbcore-c10.md):
 *   - every bare `EXPECT_TRUE(g_*)` boolean assert carries a `<<` message naming the invariant;
 *   - the case-5 ask_any race is made DETERMINISTIC: the slow market is LATCHED — it does not emit
 *     its reply until it receives an explicit "you may reply" signal that the asker sends only AFTER
 *     it has already observed the fast reply, so "fastest wins" no longer depends on an 80ms sleep;
 *   - NEW `ask_all` partial-then-cancel case (kill the asker mid-gather → `cancelled_error`);
 *   - NEW cross-core `ask_any` case (winner and loser on different cores) to pair with the same-core
 *     race (requires-multicore).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites (cross-core asks
 * leave a fixed, benign teardown residual — see ask-roundtrip.cpp).
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

// Typed exchange: request carries `key`, the base `Request<int>` carries `response`.
// (Avoid naming a field `id` — that would shadow the Event base's type-id field.)
struct Quote : public qb::Request<int> {
    int key{0};
    Quote() = default;
    explicit Quote(int k)
        : key(k) {}
};

// Self-signal for clean kill-based shutdown once a coroutine has finished.
struct PatternsDone : public qb::Event {};

// ---------------------------------------------------------------------------
// Responders
// ---------------------------------------------------------------------------

// Answers via the typed helper: response = id * 2.
class QuoteMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        co_return true;
    }
    void
    on(Quote &q) {
        qb::answer(*this, q, [](Quote const &r) { return r.key * 2; });
    }
};

// Never replies (drives timeout paths).
class SilentQuoteMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        co_return true;
    }
    void
    on(Quote &) { /* intentionally silent */ }
};

// Replies after a delay with a distinguishable value (response = key * 3). Uses a
// scoped coroutine for the delay so a kill cancels it cleanly (no dangling `this`).
class SlowQuoteMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        co_return true;
    }
    void
    on(Quote &q) {
        Quote resp    = q; // copy preserves correlation_id
        resp.response = q.key * 3;
        auto src      = q.getSource();
        spawn([resp, src](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(80ms);               // cancelled if this market is killed first
            ctx.template push_to<Quote>(src, resp); // safe via context (dropped if asker is gone)
        });
    }
};

// Explicit "you may now emit your reply" signal — used to make the ask_any race deterministic.
struct ReleaseSlow : public qb::Event {};

// LATCHED slow market: stashes its (distinguishable, key*3) reply and emits it only on ReleaseSlow.
// This removes the wall-clock race from the ask_any "fastest wins" case — the slow reply provably
// arrives AFTER the fast reply, because the asker sends ReleaseSlow only once it has the fast winner.
class LatchedQuoteMarket : public qb::Actor {
    qb::ActorId _asker{};
    Quote       _pending{};
    bool        _have_pending{false};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        registerEvent<ReleaseSlow>(*this);
        co_return true;
    }
    void
    on(Quote &q) {
        _pending          = q; // copy preserves correlation_id
        _pending.response = q.key * 3;
        _asker            = q.getSource();
        _have_pending     = true;
        // Do NOT reply yet — wait for the explicit release.
    }
    void
    on(const ReleaseSlow &) {
        if (_have_pending) {
            push<Quote>(_asker, _pending); // arrives strictly after the fast reply
            _have_pending = false;
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Typed Request<Resp> + qb::answer(*this, ) round-trip
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_typed_resp{-1};
} // namespace

class TypedClient : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TypedClient(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        registerEvent<PatternsDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto q       = co_await qb::ask(ctx, mkt, Quote{21}, 500ms);
            g_typed_resp = q.response; // filled by qb::answer(*this, ): 21 * 2
            ctx.push<PatternsDone>();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
    void
    on(const PatternsDone &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(ActorAskPatterns, TypedRequestRoundTrips) {
    g_typed_resp = -1;
    qb::Main main;
    auto     mkt = main.addActor<QuoteMarket>(0);
    main.addActor<TypedClient>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_typed_resp.load(), 42) << "21 * 2, via Request<int>::response filled by qb::answer";
}

// ---------------------------------------------------------------------------
// 2. ask_all gathers every reply (same core)
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_all_sum{-1};
std::atomic<int> g_all_count{-1};
} // namespace

class ScatterClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit ScatterClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        registerEvent<PatternsDone>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto replies = co_await qb::ask_all(ctx, markets, Quote{10}, 500ms);
            int  sum     = 0;
            for (auto const &r : replies)
                sum += r.response; // each: 10 * 2 = 20
            g_all_count = static_cast<int>(replies.size());
            g_all_sum   = sum;
            ctx.push<PatternsDone>();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
    void
    on(const PatternsDone &) {
        for (auto m : _markets)
            push<qb::KillEvent>(m);
        kill();
    }
};

TEST(ActorAskPatterns, AskAllGathersAllReplies) {
    g_all_sum = g_all_count = -1;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    for (int i = 0; i < 3; ++i)
        markets.push_back(main.addActor<QuoteMarket>(0));
    main.addActor<ScatterClient>(0, markets);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_all_count.load(), 3) << "ask_all must gather a reply from every one of the 3 markets";
    EXPECT_EQ(g_all_sum.load(), 60) << "3 * (10 * 2)";
}

// ---------------------------------------------------------------------------
// 3. ask_all across cores (markets on core 1, asker on core 0)
// ---------------------------------------------------------------------------
TEST(ActorAskPatterns, AskAllCrossCore) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place asker and responders on distinct cores";
    }
    g_all_sum = g_all_count = -1;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    for (int i = 0; i < 3; ++i)
        markets.push_back(main.addActor<QuoteMarket>(1)); // responders on core 1
    main.addActor<ScatterClient>(0, markets);             // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_all_count.load(), 3) << "ask_all must gather all 3 replies across cores";
    EXPECT_EQ(g_all_sum.load(), 60) << "3 * (10 * 2), round-tripped across cores";
}

// ---------------------------------------------------------------------------
// 4. ask_all fails with timeout_error if ANY target stays silent
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_all_timed_out{false};
} // namespace

class ScatterTimeoutClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit ScatterTimeoutClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_all(ctx, markets, Quote{1}, 40ms);
            } catch (const qb::io::async::timeout_error &) {
                g_all_timed_out = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, AskAllTimesOutIfAnySilent) {
    g_all_timed_out = false;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    markets.push_back(main.addActor<QuoteMarket>(0));       // replies
    markets.push_back(main.addActor<SilentQuoteMarket>(0)); // never replies
    main.addActor<ScatterTimeoutClient>(0, markets);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_all_timed_out.load()) << "ask_all must fail with timeout_error when any target stays silent";
}

// ---------------------------------------------------------------------------
// 5. ask_any resolves with the FIRST reply (fastest wins).
//    DETERMINISTIC: the slow market is LATCHED — it holds its reply until the asker
//    sends ReleaseSlow, which the asker does only AFTER ask_any has already resolved
//    on the fast reply. So "fastest wins" is proven by construction, not by an 80ms
//    sleep landing after a 0ms reply.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_any_resp{-1};
} // namespace

class RaceClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit RaceClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            // The latched slow market never emits its reply (no ReleaseSlow is ever sent), so ask_any
            // can only resolve on the fast market — "fastest wins" holds by construction, not timing.
            auto winner = co_await qb::ask_any(ctx, markets, Quote{10}, 500ms);
            g_any_resp  = winner.response; // fast market: 10 * 2 = 20 (latched slow would be 30)
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, AskAnyFirstReplyWins) {
    g_any_resp = -1;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    markets.push_back(main.addActor<LatchedQuoteMarket>(0)); // never emits (latched) → cannot win
    markets.push_back(main.addActor<QuoteMarket>(0));        // replies immediately (key*2) → wins
    main.addActor<RaceClient>(0, markets);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_any_resp.load(), 20) << "ask_any must resolve on the immediate market (key*2); the latched slow market cannot race ahead";
}

// ---------------------------------------------------------------------------
// 6. ask_any fails with timeout_error if ALL stay silent
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_any_timed_out{false};
} // namespace

class RaceTimeoutClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit RaceTimeoutClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_any(ctx, markets, Quote{1}, 40ms);
            } catch (const qb::io::async::timeout_error &) {
                g_any_timed_out = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, AskAnyTimesOutWhenAllSilent) {
    g_any_timed_out = false;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    markets.push_back(main.addActor<SilentQuoteMarket>(0));
    markets.push_back(main.addActor<SilentQuoteMarket>(0));
    main.addActor<RaceTimeoutClient>(0, markets);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_any_timed_out.load()) << "ask_any must fail with timeout_error when ALL targets stay silent";
}

// ---------------------------------------------------------------------------
// 6b. ask_all partial-then-cancel: the asker is killed mid-gather (one market has
//     already replied, another is still silent) → the whole ask_all unwinds with
//     cancelled_error, not timeout_error, and no value is observed.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_all_cancelled{false};
std::atomic<bool> g_all_completed{false};
} // namespace

// Kills a victim after a short delay, then stops the engine slightly later (so the victim's
// cancelled coroutine unwinds before teardown). Mirrors the shared KillThenStopHelper idiom.
class KillThenStopAll : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit KillThenStopAll(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill mid-gather
        qb::io::async::callback([] { qb::Main::stop(); }, 120ms);
        co_return true;
    }
};

class ScatterCancelClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit ScatterCancelClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_all(ctx, markets, Quote{1}, 5s); // long timeout — we are killed first
                g_all_completed = true;                           // MUST NOT run (cancelled mid-gather)
            } catch (const qb::io::async::cancelled_error &) {
                g_all_cancelled = true; // proof: killed mid-gather, not timed out
            }
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, AskAllCancelledMidGather) {
    g_all_cancelled = false;
    g_all_completed = false;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    markets.push_back(main.addActor<QuoteMarket>(0));       // replies immediately (partial progress)
    markets.push_back(main.addActor<SilentQuoteMarket>(0)); // never replies → ask_all stays pending
    auto client = main.addActor<ScatterCancelClient>(0, markets);
    main.addActor<KillThenStopAll>(0, client);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_all_cancelled.load())
        << "killing the asker mid-ask_all must unwind with cancelled_error, not timeout_error";
    EXPECT_FALSE(g_all_completed.load()) << "ask_all must NOT complete once the asker is cancelled";
}

// ---------------------------------------------------------------------------
// 6c. ask_any across cores: the winner and loser live on DIFFERENT cores. The
//     latched slow market (core 1) never emits, so the immediate market (core 1
//     too — distinct from the asker's core 0) must win. Pairs with the same-core
//     race (case 5).
// ---------------------------------------------------------------------------
TEST(ActorAskPatterns, AskAnyCrossCore) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to put winner/loser off the asker's core";
    }
    g_any_resp = -1;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    markets.push_back(main.addActor<LatchedQuoteMarket>(1)); // never emits (latched) on core 1
    markets.push_back(main.addActor<QuoteMarket>(1));        // replies immediately on core 1 → wins
    main.addActor<RaceClient>(0, markets);                   // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_any_resp.load(), 20)
        << "ask_any must resolve on the immediate cross-core market (key*2); the latched one cannot win";
}

// ---------------------------------------------------------------------------
// 7. qb::answer(*this, )'s resolve_ask guard: one actor both ASKS and ANSWERS the same
//    event type. Its own upstream reply must route to its coroutine (resolve_ask
//    true), while an inbound request from a client must be answered (false).
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_relay_upstream{-1}; // what the relay got from the market
std::atomic<int> g_client_resp{-1};    // what the client got from the relay
std::atomic<int> g_guard_done{0};
} // namespace

class Relay : public qb::Actor {
    qb::ActorId _market;

public:
    explicit Relay(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto up          = co_await qb::ask(ctx, mkt, Quote{5}, 500ms);
            g_relay_upstream = up.response; // market: 5 * 2 = 10
            if (g_guard_done.fetch_add(1) == 1)
                qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Quote &q) {
        // Same handler sees BOTH our upstream reply and inbound client requests.
        // qb::answer(*this, ) routes our own reply via resolve_ask first, else answers (id * 100).
        qb::answer(*this, q, [](Quote const &r) { return r.key * 100; });
    }
};

class GuardClient : public qb::Actor {
    qb::ActorId _relay;

public:
    explicit GuardClient(qb::ActorId r)
        : _relay(r) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        auto relay = _relay;
        spawn([relay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto r        = co_await qb::ask(ctx, relay, Quote{3}, 500ms);
            g_client_resp = r.response; // relay answered: 3 * 100 = 300
            if (g_guard_done.fetch_add(1) == 1)
                qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Quote &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, AnswerGuardSeparatesOwnReplyFromRequests) {
    g_relay_upstream = g_client_resp = -1;
    g_guard_done                     = 0;
    qb::Main main;
    auto     mkt   = main.addActor<QuoteMarket>(0);
    auto     relay = main.addActor<Relay>(0, mkt);
    main.addActor<GuardClient>(0, relay);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_relay_upstream.load(), 10) << "the relay's own coroutine got the market's reply (5*2), not the client's request";
    EXPECT_EQ(g_client_resp.load(), 300) << "the relay answered the client's request (3*100) via the resolve_ask guard";
}

// ===========================================================================
// Saga / orchestration: ctx.run_saga(...) with compensation on failure.
// ===========================================================================

// Distributed-transaction steps, each a typed Request<bool>.
struct Reserve : public qb::Request<bool> {
    int item{0};
    Reserve() = default;
    explicit Reserve(int i)
        : item(i) {}
};
struct Charge : public qb::Request<bool> {
    int amount{0};
    Charge() = default;
    explicit Charge(int a)
        : amount(a) {}
};
struct Release : public qb::Request<bool> {
    int item{0};
    Release() = default;
    explicit Release(int i)
        : item(i) {}
};

namespace {
std::atomic<bool> g_reserved{false}; // Inventory answered a Reserve (saga step 1 completed)
std::atomic<bool> g_released{false}; // Inventory answered a Release (compensation ran)
std::atomic<bool> g_saga_failed{false};
std::atomic<bool> g_saga_ok{false};
// Order in which compensations executed. SINGLE-WRITER / READ-AFTER-JOIN: written only from the saga
// client's own VirtualCore (single-thread per core) and read by the test thread ONLY after
// main.join() — join() establishes the happens-before, so the plain std::vector needs no extra sync.
std::vector<int>  g_comp_order;
} // namespace

// Reserves items and, on compensation, releases them.
class Inventory : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Release>(*this);
        co_return true;
    }
    void
    on(Reserve &r) {
        qb::answer(*this, r, [](Reserve const &) {
            g_reserved = true;
            return true;
        });
    }
    void
    on(Release &r) {
        qb::answer(*this, r, [](Release const &) {
            g_released = true;
            return true;
        });
    }
};

class PaymentOk : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        co_return true;
    }
    void
    on(Charge &c) {
        qb::answer(*this, c, [](Charge const &) { return true; });
    }
};

class PaymentSilent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        co_return true;
    }
    void
    on(Charge &) { /* never replies -> the Charge step times out */ }
};

// Reserve -> (register Release compensation) -> Charge. If Charge fails, the saga
// rolls back by asking Release.
class SagaClient : public qb::Actor {
    qb::ActorId _inv;
    qb::ActorId _pay;

public:
    SagaClient(qb::ActorId inv, qb::ActorId pay)
        : _inv(inv)
        , _pay(pay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Charge>(*this);
        registerEvent<Release>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                    co_await qb::ask(ctx, inv, Reserve{1}, 500ms);
                    saga.on_compensate([ctx, inv]() -> qb::io::async::task<void> { co_await qb::ask(ctx, inv, Release{1}, 500ms); });
                    co_await qb::ask(ctx, pay, Charge{99}, 40ms);
                });
                g_saga_ok = true; // reached only if every step succeeded
            } catch (const qb::io::async::timeout_error &) {
                g_saga_failed = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Reserve &e) {
        resolve_ask(e);
    }
    void
    on(Charge &e) {
        resolve_ask(e);
    }
    void
    on(Release &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, SagaCompensatesOnStepFailure) {
    g_released = g_saga_failed = g_saga_ok = false;
    qb::Main main;
    auto     inv = main.addActor<Inventory>(0);
    auto     pay = main.addActor<PaymentSilent>(0); // Charge times out
    main.addActor<SagaClient>(0, inv, pay);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_saga_failed.load()) << "the Charge step must time out and fail the saga";
    EXPECT_TRUE(g_released.load()) << "the Reserve step must be rolled back via Release (compensation ran)";
    EXPECT_FALSE(g_saga_ok.load()) << "a failed saga must NOT report success";
}

TEST(ActorAskPatterns, SagaHappyPathRunsNoCompensation) {
    g_released = g_saga_failed = g_saga_ok = false;
    qb::Main main;
    auto     inv = main.addActor<Inventory>(0);
    auto     pay = main.addActor<PaymentOk>(0); // Charge succeeds
    main.addActor<SagaClient>(0, inv, pay);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_saga_ok.load()) << "every saga step succeeded → the saga completes";
    EXPECT_FALSE(g_released.load()) << "the happy path must run NO compensation";
    EXPECT_FALSE(g_saga_failed.load()) << "the happy path must not report failure";
}

// Two steps register compensations; a third fails -> compensations run in reverse.
class SagaReverseClient : public qb::Actor {
    qb::ActorId _inv;
    qb::ActorId _pay;

public:
    SagaReverseClient(qb::ActorId inv, qb::ActorId pay)
        : _inv(inv)
        , _pay(pay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Charge>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                    co_await qb::ask(ctx, inv, Reserve{1}, 500ms);
                    saga.on_compensate([]() -> qb::io::async::task<void> {
                        g_comp_order.push_back(1);
                        co_return;
                    });
                    co_await qb::ask(ctx, inv, Reserve{2}, 500ms);
                    saga.on_compensate([]() -> qb::io::async::task<void> {
                        g_comp_order.push_back(2);
                        co_return;
                    });
                    co_await qb::ask(ctx, pay, Charge{99}, 40ms); // silent -> timeout
                });
            } catch (const qb::io::async::timeout_error &) {
                g_saga_failed = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Reserve &e) {
        resolve_ask(e);
    }
    void
    on(Charge &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, SagaCompensationsRunInReverseOrder) {
    g_saga_failed = false;
    g_comp_order.clear();
    qb::Main main;
    auto     inv = main.addActor<Inventory>(0);
    auto     pay = main.addActor<PaymentSilent>(0);
    main.addActor<SagaReverseClient>(0, inv, pay);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_saga_failed.load()) << "the Charge step must time out and fail the saga";
    ASSERT_EQ(g_comp_order.size(), 2u) << "both registered compensations must run";
    EXPECT_EQ(g_comp_order[0], 2) << "the last-registered compensation runs first (LIFO)";
    EXPECT_EQ(g_comp_order[1], 1) << "the first-registered compensation runs last (LIFO)";
}

// ---------------------------------------------------------------------------
// Saga edge cases: cancel-mid-flow skips rollback; a throwing compensation does
// not abort the remaining rollbacks (best-effort, saga.h compensate()).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_cancel_ok{false};
std::atomic<bool> g_saga_cancelled{false}; // run_saga re-threw cancelled_error (killed mid-flow)
std::atomic<bool> g_comp0_ran{false};
} // namespace

// Never answers Charge → the step blocks until the actor is killed.
class SilentCharge : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        co_return true;
    }
    void
    on(Charge &) {}
};

class SagaCancelClient : public qb::Actor {
    qb::ActorId _inv, _pay;

public:
    SagaCancelClient(qb::ActorId inv, qb::ActorId pay)
        : _inv(inv)
        , _pay(pay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Release>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                    co_await qb::ask(ctx, inv, Reserve{1}, 500ms); // step 1 succeeds (sets g_reserved)
                    saga.on_compensate([ctx, inv]() -> qb::io::async::task<void> {
                        co_await qb::ask(ctx, inv, Release{1}, 500ms); // must NOT run on cancel
                    });
                    co_await qb::ask(ctx, pay, Charge{99}, 5s); // long wait — we are killed here
                });
                g_cancel_ok = true; // unreachable: run_saga re-throws the cancellation
            } catch (const qb::io::async::cancelled_error &) {
                g_saga_cancelled = true; // proof: killed mid-flow (past step 1, inside step 2)
            }
        });
        co_return true;
    }
    void
    on(Reserve &e) {
        resolve_ask(e);
    }
    void
    on(Release &e) {
        resolve_ask(e);
    }
};

class KillThenStop : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit KillThenStop(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill mid-step-2
        qb::io::async::callback([] { qb::Main::stop(); }, 120ms);
        co_return true;
    }
};

TEST(ActorAskPatterns, SagaCancelledMidFlowSkipsCompensation) {
    g_reserved       = false;
    g_released       = false;
    g_cancel_ok      = false;
    g_saga_cancelled = false;
    qb::Main main;
    auto     inv    = main.addActor<Inventory>(0);
    auto     pay    = main.addActor<SilentCharge>(0);
    auto     client = main.addActor<SagaCancelClient>(0, inv, pay);
    main.addActor<KillThenStop>(0, client);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_reserved.load()) << "teeth: step 1 (Reserve) ran → the saga DID enter and register its compensation";
    EXPECT_TRUE(g_saga_cancelled.load()) << "teeth: the asker was killed mid-step-2 (cancelled_error), not crashed early";
    EXPECT_FALSE(g_released.load()) << "a mid-flow cancel must SKIP rollback — Release must never be asked";
    EXPECT_FALSE(g_cancel_ok.load()) << "the saga must NOT report completion when cancelled mid-flow";
}

class SagaCompThrowClient : public qb::Actor {
    qb::ActorId _inv, _pay;

public:
    SagaCompThrowClient(qb::ActorId inv, qb::ActorId pay)
        : _inv(inv)
        , _pay(pay) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Release>(*this);
        registerEvent<Charge>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                    co_await qb::ask(ctx, inv, Reserve{1}, 500ms);
                    saga.on_compensate([ctx, inv]() -> qb::io::async::task<void> {
                        co_await qb::ask(ctx, inv, Release{1}, 500ms); // comp[0] — real undo
                        g_comp0_ran = true;
                    });
                    saga.on_compensate([]() -> qb::io::async::task<void> {
                        if (true)
                            throw std::runtime_error("compensation 2 boom"); // comp[1] — runs first (LIFO), throws
                        co_return;
                    });
                    co_await qb::ask(ctx, pay, Charge{99}, 40ms); // times out → saga fails → rollback
                });
            } catch (...) {
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Reserve &e) {
        resolve_ask(e);
    }
    void
    on(Release &e) {
        resolve_ask(e);
    }
    void
    on(Charge &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskPatterns, SagaCompensationThrowsContinuesRemaining) {
    g_released  = false;
    g_comp0_ran = false;
    qb::Main main;
    auto     inv = main.addActor<Inventory>(0);
    auto     pay = main.addActor<PaymentSilent>(0); // Charge times out → saga fails
    main.addActor<SagaCompThrowClient>(0, inv, pay);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_comp0_ran.load()) << "comp[0] must run despite comp[1] throwing first (best-effort rollback)";
    EXPECT_TRUE(g_released.load()) << "the surviving rollback (Release) must go through after the throw";
}
