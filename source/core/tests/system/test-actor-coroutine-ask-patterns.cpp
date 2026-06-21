/**
 * @file test-actor-coroutine-ask-patterns.cpp
 * @brief Tests for the request/response composition layer built on `ask`:
 *        the typed `Request<Resp>` envelope + `qb::answer(*this, )` helper, and the
 *        `ScopedCoroContext::ask_all` / `ask_any` scatter-gather combinators.
 *
 * Validates:
 *   - `Request<Resp>` + `qb::answer(*this, e, fn)` round-trips a typed response (one event type);
 *   - `ask_all` gathers every reply (same-core and cross-core), and fails with
 *     `timeout_error` if any target stays silent;
 *   - `ask_any` resolves with the first reply (fastest wins) and `timeout_error` if all
 *     stay silent;
 *   - `qb::answer(*this, )`'s `resolve_ask` guard lets one actor both ask and answer the *same*
 *     event type without confusing its own replies with inbound requests.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites
 * (cross-core asks leave a fixed, benign teardown residual — see test-actor-coroutine-ask).
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>
#include <vector>

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
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        return true;
    }
    void
    on(Quote &q) {
        qb::answer(*this, q, [](Quote const &r) { return r.key * 2; });
    }
};

// Never replies (drives timeout paths).
class SilentQuoteMarket : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        return true;
    }
    void
    on(Quote &) { /* intentionally silent */ }
};

// Replies after a delay with a distinguishable value (response = key * 3). Uses a
// scoped coroutine for the delay so a kill cancels it cleanly (no dangling `this`).
class SlowQuoteMarket : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        return true;
    }
    void
    on(Quote &q) {
        Quote resp    = q; // copy preserves correlation_id
        resp.response = q.key * 3;
        auto src      = q.getSource();
        spawn([resp, src](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(80ms); // cancelled if this market is killed first
            ctx.template push_to<Quote>(src, resp); // safe via context (dropped if asker is gone)
        });
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
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        registerEvent<PatternsDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto q       = co_await qb::ask(ctx, mkt, Quote{21}, 500ms);
            g_typed_resp = q.response; // filled by qb::answer(*this, ): 21 * 2
            ctx.push<PatternsDone>();
        });
        return true;
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
    EXPECT_EQ(g_typed_resp.load(), 42); // 21 * 2, via Request<int>::response + qb::answer(*this, )
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
    bool
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
        return true;
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
    EXPECT_EQ(g_all_count.load(), 3);
    EXPECT_EQ(g_all_sum.load(), 60); // 3 * (10 * 2)
}

// ---------------------------------------------------------------------------
// 3. ask_all across cores (markets on core 1, asker on core 0)
// ---------------------------------------------------------------------------
TEST(ActorAskPatterns, AskAllCrossCore) {
    g_all_sum = g_all_count = -1;
    qb::Main                 main;
    std::vector<qb::ActorId> markets;
    for (int i = 0; i < 3; ++i)
        markets.push_back(main.addActor<QuoteMarket>(1)); // responders on core 1
    main.addActor<ScatterClient>(0, markets);             // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_all_count.load(), 3);
    EXPECT_EQ(g_all_sum.load(), 60);
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
    bool
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
        return true;
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
    EXPECT_TRUE(g_all_timed_out.load());
}

// ---------------------------------------------------------------------------
// 5. ask_any resolves with the FIRST reply (fastest wins)
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_any_resp{-1};
} // namespace

class RaceClient : public qb::Actor {
    std::vector<qb::ActorId> _markets;

public:
    explicit RaceClient(std::vector<qb::ActorId> m)
        : _markets(std::move(m)) {}
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        auto markets = _markets;
        spawn([markets](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto winner = co_await qb::ask_any(ctx, markets, Quote{10}, 500ms);
            g_any_resp  = winner.response; // fast market: 10 * 2 = 20 (slow would be 30)
            qb::Main::stop();
        });
        return true;
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
    markets.push_back(main.addActor<SlowQuoteMarket>(0)); // replies after 80ms (id*3)
    markets.push_back(main.addActor<QuoteMarket>(0));     // replies immediately (id*2)
    main.addActor<RaceClient>(0, markets);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_any_resp.load(), 20); // the fast (immediate) market won
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
    bool
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
        return true;
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
    EXPECT_TRUE(g_any_timed_out.load());
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
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto up          = co_await qb::ask(ctx, mkt, Quote{5}, 500ms);
            g_relay_upstream = up.response; // market: 5 * 2 = 10
            if (g_guard_done.fetch_add(1) == 1)
                qb::Main::stop();
        });
        return true;
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
    bool
    onInit() override {
        registerEvent<Quote>(*this);
        auto relay = _relay;
        spawn([relay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto r        = co_await qb::ask(ctx, relay, Quote{3}, 500ms);
            g_client_resp = r.response; // relay answered: 3 * 100 = 300
            if (g_guard_done.fetch_add(1) == 1)
                qb::Main::stop();
        });
        return true;
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
    EXPECT_EQ(g_relay_upstream.load(), 10); // relay's coroutine got the market's reply
    EXPECT_EQ(g_client_resp.load(), 300);   // relay answered the client's request
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
std::atomic<bool> g_released{false};   // Inventory answered a Release (compensation ran)
std::atomic<bool> g_saga_failed{false};
std::atomic<bool> g_saga_ok{false};
std::vector<int>  g_comp_order;        // order compensations executed (single worker thread)
} // namespace

// Reserves items and, on compensation, releases them.
class Inventory : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Release>(*this);
        return true;
    }
    void
    on(Reserve &r) {
        qb::answer(*this, r, [](Reserve const &) { return true; });
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
    bool
    onInit() override {
        registerEvent<Charge>(*this);
        return true;
    }
    void
    on(Charge &c) {
        qb::answer(*this, c, [](Charge const &) { return true; });
    }
};

class PaymentSilent : public qb::Actor {
public:
    bool
    onInit() override {
        registerEvent<Charge>(*this);
        return true;
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
    bool
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Charge>(*this);
        registerEvent<Release>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(
                    ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga)
                             -> qb::io::async::task<void> {
                        co_await qb::ask(ctx, inv, Reserve{1}, 500ms);
                        saga.on_compensate([ctx, inv]() -> qb::io::async::task<void> {
                            co_await qb::ask(ctx, inv, Release{1}, 500ms);
                        });
                        co_await qb::ask(ctx, pay, Charge{99}, 40ms);
                    });
                g_saga_ok = true; // reached only if every step succeeded
            } catch (const qb::io::async::timeout_error &) {
                g_saga_failed = true;
            }
            qb::Main::stop();
        });
        return true;
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
    EXPECT_TRUE(g_saga_failed.load()); // the Charge step timed out
    EXPECT_TRUE(g_released.load());    // the Reserve was rolled back via Release
    EXPECT_FALSE(g_saga_ok.load());
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
    EXPECT_TRUE(g_saga_ok.load());      // all steps succeeded
    EXPECT_FALSE(g_released.load());    // no rollback
    EXPECT_FALSE(g_saga_failed.load());
}

// Two steps register compensations; a third fails -> compensations run in reverse.
class SagaReverseClient : public qb::Actor {
    qb::ActorId _inv;
    qb::ActorId _pay;

public:
    SagaReverseClient(qb::ActorId inv, qb::ActorId pay)
        : _inv(inv)
        , _pay(pay) {}
    bool
    onInit() override {
        registerEvent<Reserve>(*this);
        registerEvent<Charge>(*this);
        auto inv = _inv;
        auto pay = _pay;
        spawn([inv, pay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(
                    ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga)
                             -> qb::io::async::task<void> {
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
        return true;
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
    EXPECT_TRUE(g_saga_failed.load());
    ASSERT_EQ(g_comp_order.size(), 2u);
    EXPECT_EQ(g_comp_order[0], 2); // last registered runs first (LIFO)
    EXPECT_EQ(g_comp_order[1], 1);
}
