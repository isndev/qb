/**
 * @file test-actor-coroutine-ask.cpp
 * @brief Tests for the native `ask` request/response pattern (Layer 3).
 *
 * Validates: a coroutine `co_await qb::ask(ctx, target, req, timeout)` resolves with the
 * responder's reply (correlation round-trips via `reply()`); a non-responding target
 * yields `timeout_error`; and killing the asker while it waits yields `cancelled_error`.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int>  g_ask_price{-1};
std::atomic<bool> g_ask_timed_out{false};
std::atomic<bool> g_ask_cancelled{false};
std::atomic<bool> g_trader_body_done{false};

void
reset_flags() {
    g_ask_price     = -1;
    g_ask_timed_out = false;
    g_ask_cancelled = false;
}
} // namespace

// Single exchange event: the responder fills `price` and reply()s it back.
struct PriceQuery : public qb::AskEvent {
    int query{0};
    int price{0};
    PriceQuery() = default;
    explicit PriceQuery(int q)
        : query(q) {}
};

// Responder that answers.
class Market : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        co_return true;
    }
    void
    on(PriceQuery &q) {
        q.price = q.query * 2; // compute the response
        reply(q);              // route back to the asker, preserving correlation_id
    }
};

// Responder that never answers (drives timeout / cancel paths).
class SilentMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        co_return true;
    }
    void
    on(PriceQuery &) { /* intentionally no reply */ }
};

// Self-signal used to shut an asker down cleanly once its coroutine has finished
// (kill-based shutdown reaps completed coroutine frames; calling qb::Main::stop()
// from inside a coroutine can leave a just-completed frame un-drained at teardown).
struct AskDone : public qb::Event {};

// ---------------------------------------------------------------------------
// 1. ask success
// ---------------------------------------------------------------------------
class TraderOk : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderOk(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        registerEvent<AskDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto r      = co_await qb::ask(ctx, mkt, PriceQuery{21}, 500ms);
            g_ask_price = r.price;
            ctx.push<AskDone>(); // shut down via the actor's own handler, cleanly
            g_trader_body_done = true;
        });
        co_return true;
    }
    void
    on(PriceQuery &e) {
        resolve_ask(e); // route responses to the waiting coroutine
    }
    void
    on(const AskDone &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(ActorCoroutineAsk, AskSucceeds) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<Market>(0);
    main.addActor<TraderOk>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_ask_price.load(), 42); // 21 * 2, round-tripped through reply()
}

// ---------------------------------------------------------------------------
// 2. ask timeout
// ---------------------------------------------------------------------------
class TraderTimeout : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderTimeout(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, PriceQuery{7}, 40ms);
            } catch (const qb::io::async::timeout_error &) {
                g_ask_timed_out = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(PriceQuery &e) {
        resolve_ask(e);
    }
};

TEST(ActorCoroutineAsk, AskTimesOutWhenNoReply) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<TraderTimeout>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load());
    EXPECT_EQ(g_ask_price.load(), -1);
}

// Timeout across cores: the ev_timer that backs the timeout lives on the asker's
// core, so a silent responder on another core still times out correctly.
TEST(ActorCoroutineAsk, AskTimesOutCrossCore) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(1); // silent responder on core 1
    main.addActor<TraderTimeout>(0, mkt);          // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load());
    EXPECT_EQ(g_ask_price.load(), -1);
}

// ---------------------------------------------------------------------------
// 3. ask cancelled when the asker is killed mid-wait
// ---------------------------------------------------------------------------
class TraderCancel : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderCancel(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, PriceQuery{99}, 5s); // long wait
            } catch (const qb::io::async::cancelled_error &) {
                g_ask_cancelled = true;
            }
            qb::Main::stop(); // ends the engine (SilentMarket is still alive)
        });
        // Kill this asker while the ask is still pending.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            30ms);
        co_return true;
    }
    void
    on(PriceQuery &e) {
        resolve_ask(e);
    }
};

TEST(ActorCoroutineAsk, AskCancelledOnKill) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<TraderCancel>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_cancelled.load());
    EXPECT_EQ(g_ask_price.load(), -1);
}

// Cancellation across cores: the asker (core 0) is killed while waiting on a
// reply from a responder on core 1. All the ask machinery (scope token, awaiter,
// timer, registry) is asker-core-local — only the request crossed cores — so the
// cancel must still wake the coroutine and unwind cleanly.
TEST(ActorCoroutineAsk, AskCancelledOnKillCrossCore) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(1); // silent responder on core 1
    main.addActor<TraderCancel>(0, mkt);           // asker on core 0, killed mid-wait
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_cancelled.load());
    EXPECT_EQ(g_ask_price.load(), -1);
}

// ---------------------------------------------------------------------------
// 4. ask across VirtualCores (asker on core 0, responder on core 1)
//    The correlation registry and awaiter are core-local; only the request and
//    reply cross cores through the normal lock-free pipes.
// ---------------------------------------------------------------------------
TEST(ActorCoroutineAsk, AskAcrossCores) {
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<Market>(1); // responder on core 1
    main.addActor<TraderOk>(0, mkt);         // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_ask_price.load(), 42);      // round-tripped across cores
    EXPECT_TRUE(g_trader_body_done.load()); // body coroutine ran to completion
}

// ---------------------------------------------------------------------------
// N sequential cross-core asks from a single coroutine.
//
// Besides exercising back-to-back cross-core round-trips, this is the guard
// against a *per-call* coroutine-frame leak: N asks here leak the same fixed
// 3 frames as a single ask (the last in-flight chain, reclaimed by the OS at
// process exit — a benign teardown artifact shared with the pre-existing
// coroutine suites, not a per-call leak). Run under ASAN_OPTIONS=detect_leaks=0
// in CI, matching the rest of the actor-coroutine tests.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_seq_sum{0};
} // namespace

class TraderSeq : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderSeq(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        registerEvent<AskDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            int sum = 0;
            for (int i = 1; i <= 5; ++i) {
                auto r = co_await qb::ask(ctx, mkt, PriceQuery{i}, 500ms);
                sum += r.price; // i * 2
            }
            g_seq_sum = sum; // 2+4+6+8+10 = 30
            ctx.push<AskDone>();
        });
        co_return true;
    }
    void
    on(PriceQuery &e) {
        resolve_ask(e);
    }
    void
    on(const AskDone &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(ActorCoroutineAsk, SequentialCrossCoreAsks) {
    g_seq_sum = 0;
    qb::Main main;
    auto     mkt = main.addActor<Market>(1); // responder on core 1
    main.addActor<TraderSeq>(0, mkt);        // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_seq_sum.load(), 30);
}

// ---------------------------------------------------------------------------
// 5. Concurrent asks from one actor resolve to their own distinct responses
//    (validates correlation-id isolation in the per-core registry).
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_multi[3];
std::atomic<int> g_multi_done{0};
} // namespace

class TraderMulti : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderMulti(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        auto mkt = _market;
        for (int i = 0; i < 3; ++i) {
            spawn([mkt, i](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                auto r     = co_await qb::ask(ctx, mkt, PriceQuery{(i + 1) * 10}, 500ms);
                g_multi[i] = r.price;
                if (g_multi_done.fetch_add(1) == 2)
                    qb::Main::stop();
            });
        }
        co_return true;
    }
    void
    on(PriceQuery &e) {
        resolve_ask(e);
    }
};

TEST(ActorCoroutineAsk, ConcurrentAsksResolveDistinctly) {
    g_multi[0] = g_multi[1] = g_multi[2] = 0;
    g_multi_done                         = 0;
    qb::Main main;
    auto     mkt = main.addActor<Market>(0);
    main.addActor<TraderMulti>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_multi[0].load(), 20);
    EXPECT_EQ(g_multi[1].load(), 40);
    EXPECT_EQ(g_multi[2].load(), 60);
}

// ---------------------------------------------------------------------------
// 6. A reply arriving AFTER the ask timed out is delivered as unsolicited
//    (no double-resume / use-after-free); resolve_ask returns false.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_late_unsolicited{false};
} // namespace

class SlowMarket : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        co_return true;
    }
    void
    on(PriceQuery &q) {
        PriceQuery resp = q; // copy keeps correlation_id
        resp.price      = q.query * 2;
        auto src        = q.getSource();
        qb::io::async::callback(
            [this, resp, src] {
                if (is_alive())
                    push<PriceQuery>(src, resp); // reply long after the asker's timeout
            },
            80ms);
    }
};

class TraderLate : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderLate(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, PriceQuery{5}, 30ms);
            } catch (const qb::io::async::timeout_error &) {
                g_ask_timed_out = true;
            }
            // Do NOT stop — wait for the late reply to arrive as unsolicited.
        });
        co_return true;
    }
    void
    on(PriceQuery &e) {
        if (resolve_ask(e))
            return; // late reply: the slot is gone → false
        g_late_unsolicited = true;
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAsk, LateReplyAfterTimeoutIsUnsolicitedAndSafe) {
    reset_flags();
    g_late_unsolicited = false;
    qb::Main main;
    auto     mkt = main.addActor<SlowMarket>(0);
    main.addActor<TraderLate>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load());
    EXPECT_TRUE(g_late_unsolicited.load());
}

// ---------------------------------------------------------------------------
// 7. resolve_ask returns false for an event that is not a pending ask.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_unsolicited_false{false};
} // namespace

class Receiver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PriceQuery>(*this);
        co_return true;
    }
    void
    on(PriceQuery &e) {
        g_unsolicited_false = !resolve_ask(e); // correlation_id == 0 → no pending ask → false
        qb::Main::stop();
    }
};

class Sender : public qb::Actor {
    qb::ActorId _to;

public:
    explicit Sender(qb::ActorId t)
        : _to(t) {}
    qb::io::async::task<bool>
    onInit() override {
        push<PriceQuery>(_to, 1); // plain event, correlation_id stays 0
        co_return true;
    }
};

TEST(ActorCoroutineAsk, ResolveAskFalseForUnsolicited) {
    g_unsolicited_false = false;
    qb::Main main;
    auto     rcv = main.addActor<Receiver>(0);
    main.addActor<Sender>(0, rcv);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_unsolicited_false.load());
}

// ---------------------------------------------------------------------------
// 8. ask round-trips a non-trivial (owning) payload correctly.
// ---------------------------------------------------------------------------
struct Echo : public qb::AskEvent {
    std::shared_ptr<std::string> in;
    std::shared_ptr<std::string> out; // filled by responder
};
namespace {
std::atomic<bool> g_echo_ok{false};
} // namespace

class Echoer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        co_return true;
    }
    void
    on(Echo &e) {
        e.out = std::make_shared<std::string>("reply:" + *e.in);
        reply(e);
    }
};

class EchoClient : public qb::Actor {
    qb::ActorId _to;

public:
    explicit EchoClient(qb::ActorId t)
        : _to(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Echo>(*this);
        auto to = _to;
        spawn([to](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            Echo req;
            req.in    = std::make_shared<std::string>("hello");
            auto r    = co_await qb::ask(ctx, to, std::move(req), 500ms);
            g_echo_ok = (r.out && *r.out == "reply:hello");
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Echo &e) {
        resolve_ask(e);
    }
};

TEST(ActorCoroutineAsk, AskRoundTripsNonTrivialPayload) {
    g_echo_ok = false;
    qb::Main main;
    auto     echoer = main.addActor<Echoer>(0);
    main.addActor<EchoClient>(0, echoer);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_echo_ok.load());
}
