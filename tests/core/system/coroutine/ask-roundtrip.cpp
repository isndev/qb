/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/ask-roundtrip.cpp
 * @brief The native `ask` request/response round-trip (Layer 3) on the live event loop.
 *
 * Proves the coroutine RPC primitive `co_await qb::ask(ctx, target, req, timeout)`:
 *   - SUCCESS — the responder fills `req.response` and `qb::answer` routes it back; the value the
 *     coroutine resolves to is framework-computed (`seq * 2`), not a value the asker handed in;
 *   - TIMEOUT — a non-responding target yields `timeout_error` (same-core and cross-core: the timer
 *     that backs the timeout lives on the asker's core, so a silent responder elsewhere still fires);
 *   - CANCEL — killing the asker mid-wait yields `cancelled_error` and unwinds cleanly (same-core and
 *     cross-core: the scope token / awaiter / timer / registry are all asker-core-local);
 *   - CROSS-CORE round-trip — request and reply traverse the lock-free pipes, correlation preserved;
 *   - CORRELATION isolation — N sequential and concurrent asks from one actor each resolve to their
 *     own distinct reply;
 *   - LATE REPLY — a reply arriving AFTER the ask timed out is delivered as unsolicited
 *     (`resolve_ask` returns false), with no double-resume / use-after-free, same-core AND cross-core;
 *   - NON-TRIVIAL payload — an owning (`shared_ptr<string>`) request round-trips intact.
 *
 * Responders come from the shared zoo `shared/AskResponders.h` (`Market` / `SilentMarket` /
 * `SlowMarket` / `Echoer`, all over the typed `Ping : Request<int>` exchange) — the single source of
 * truth, deduped from the byte-for-byte clones that used to live in this file and ask-patterns.cpp.
 *
 * Hardening over the original (see dev/tests-audit/qb-core/qbcore-c10.md):
 *   - the fragile `30ms`-timeout / `80ms`-late-reply ORDERING ORACLE is replaced by an EVENT-DRIVEN
 *     gate: the gated market holds its reply until it receives an explicit "now" signal that the
 *     asker sends only AFTER it has observed its own timeout, so the late-reply-is-unsolicited
 *     invariant no longer depends on a wall-clock race;
 *   - a new cross-core timeout-mid-reply case (requires-multicore) exercises the cross-core teardown
 *     race: the reply is in flight from core 1 when the asker's core-0 timeout fires.
 *
 * Every in-coroutine / in-handler effect is mirrored to a file-scope atomic asserted AFTER `join()`
 * (no pass-if-never-run). Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine
 * suites (cross-core asks leave a fixed, benign teardown residual).
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

#include "../../shared/AskResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::Echoer;
using qb::test::Market;
using qb::test::Ping;
using qb::test::SilentMarket;
using qb::test::SlowMarket;

namespace {
std::atomic<int>  g_ask_price{-1};
std::atomic<bool> g_ask_timed_out{false};
std::atomic<bool> g_ask_cancelled{false};
std::atomic<bool> g_trader_body_done{false};

void
reset_flags() {
    g_ask_price        = -1;
    g_ask_timed_out    = false;
    g_ask_cancelled    = false;
    g_trader_body_done = false;
}
} // namespace

// Self-signal used to shut an asker down cleanly once its coroutine has finished
// (kill-based shutdown reaps completed coroutine frames; calling qb::Main::stop()
// from inside a coroutine can leave a just-completed frame un-drained at teardown).
struct AskDone : public qb::Event {};

// ---------------------------------------------------------------------------
// 1. ask success — the resolved value is framework-computed (seq * 2).
// ---------------------------------------------------------------------------
class TraderOk : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderOk(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<AskDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto r             = co_await qb::ask(ctx, mkt, Ping{21}, 500ms);
            g_ask_price        = r.response; // 21 * 2, round-tripped through qb::answer
            g_trader_body_done = true;
            ctx.push<AskDone>(); // shut down via the actor's own handler, cleanly
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e); // route the response to the waiting coroutine
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
    EXPECT_EQ(g_ask_price.load(), 42) << "21 * 2, round-tripped through qb::answer";
    EXPECT_TRUE(g_trader_body_done.load()) << "the asker coroutine must have resolved and run to completion";
}

// ---------------------------------------------------------------------------
// 2. ask timeout (same-core and cross-core).
// ---------------------------------------------------------------------------
class TraderTimeout : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderTimeout(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{7}, 40ms);
            } catch (const qb::io::async::timeout_error &) {
                g_ask_timed_out = true;
            }
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
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
    EXPECT_TRUE(g_ask_timed_out.load()) << "a silent responder must drive the ask to timeout_error";
    EXPECT_EQ(g_ask_price.load(), -1) << "no value may be observed when the ask times out";
}

// Timeout across cores: the qev_timer that backs the timeout lives on the asker's
// core, so a silent responder on another core still times out correctly.
TEST(ActorCoroutineAsk, AskTimesOutCrossCore) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place asker and responder on distinct cores";
    }
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(1); // silent responder on core 1
    main.addActor<TraderTimeout>(0, mkt);          // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load()) << "a cross-core silent responder must still time out (asker-core timer)";
    EXPECT_EQ(g_ask_price.load(), -1);
}

// ---------------------------------------------------------------------------
// 3. ask cancelled when the asker is killed mid-wait (same-core and cross-core).
// ---------------------------------------------------------------------------
class TraderCancel : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderCancel(qb::ActorId m)
        : _market(m) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{99}, 5s); // long wait
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
    on(Ping &e) {
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
    EXPECT_TRUE(g_ask_cancelled.load()) << "killing the asker mid-wait must surface cancelled_error";
    EXPECT_EQ(g_ask_price.load(), -1);
}

// Cancellation across cores: the asker (core 0) is killed while waiting on a reply from a responder
// on core 1. All the ask machinery (scope token, awaiter, timer, registry) is asker-core-local — only
// the request crossed cores — so the cancel must still wake the coroutine and unwind cleanly.
TEST(ActorCoroutineAsk, AskCancelledOnKillCrossCore) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place asker and responder on distinct cores";
    }
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(1); // silent responder on core 1
    main.addActor<TraderCancel>(0, mkt);           // asker on core 0, killed mid-wait
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_cancelled.load()) << "a cross-core ask must still cancel cleanly on kill";
    EXPECT_EQ(g_ask_price.load(), -1);
}

// ---------------------------------------------------------------------------
// 4. ask across VirtualCores (asker on core 0, responder on core 1). The
//    correlation registry and awaiter are core-local; only request+reply cross.
// ---------------------------------------------------------------------------
TEST(ActorCoroutineAsk, AskAcrossCores) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place asker and responder on distinct cores";
    }
    reset_flags();
    qb::Main main;
    auto     mkt = main.addActor<Market>(1); // responder on core 1
    main.addActor<TraderOk>(0, mkt);         // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_ask_price.load(), 42) << "round-tripped across cores: 21 * 2";
    EXPECT_TRUE(g_trader_body_done.load()) << "the body coroutine ran to completion across cores";
}

// ---------------------------------------------------------------------------
// N sequential cross-core asks from a single coroutine — back-to-back round-trips.
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
        registerEvent<Ping>(*this);
        registerEvent<AskDone>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            int sum = 0;
            for (int i = 1; i <= 5; ++i) {
                auto r = co_await qb::ask(ctx, mkt, Ping{i}, 500ms);
                sum += r.response; // i * 2
            }
            g_seq_sum = sum; // 2+4+6+8+10 = 30
            ctx.push<AskDone>();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const AskDone &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(ActorCoroutineAsk, SequentialCrossCoreAsks) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place asker and responder on distinct cores";
    }
    g_seq_sum = 0;
    qb::Main main;
    auto     mkt = main.addActor<Market>(1); // responder on core 1
    main.addActor<TraderSeq>(0, mkt);        // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_seq_sum.load(), 30) << "five back-to-back asks: 2+4+6+8+10";
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
        registerEvent<Ping>(*this);
        auto mkt = _market;
        for (int i = 0; i < 3; ++i) {
            spawn([mkt, i](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                auto r     = co_await qb::ask(ctx, mkt, Ping{(i + 1) * 10}, 500ms);
                g_multi[i] = r.response;
                if (g_multi_done.fetch_add(1) == 2)
                    qb::Main::stop();
            });
        }
        co_return true;
    }
    void
    on(Ping &e) {
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
    EXPECT_EQ(g_multi[0].load(), 20) << "ask(10) → 20";
    EXPECT_EQ(g_multi[1].load(), 40) << "ask(20) → 40";
    EXPECT_EQ(g_multi[2].load(), 60) << "ask(30) → 60";
}

// ---------------------------------------------------------------------------
// 6. A reply arriving AFTER the ask timed out is delivered as unsolicited (no
//    double-resume / use-after-free); resolve_ask returns false.
//
//    DE-FLAKED: the old version raced a 30ms ask timeout against an 80ms late
//    wall-clock reply. Here the market (GatedMarket) parks the reply and releases
//    it ONLY when it receives an explicit "ReleaseReply" signal, which the asker
//    sends from inside its timeout catch — so the reply provably arrives AFTER the
//    timeout, no wall-clock margin involved.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_late_unsolicited{false};

// Explicit "you may now reply" signal sent by the asker once it has timed out.
struct ReleaseReply : public qb::Event {};
} // namespace

// Holds the (correlation-preserving) reply for a pending Ping until told to release it.
class GatedMarket : public qb::Actor {
    qb::ActorId _asker{};
    Ping        _pending{};
    bool        _have_pending{false};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<ReleaseReply>(*this);
        co_return true;
    }
    void
    on(Ping &q) {
        // Stash the reply; do NOT answer yet. Wait for the asker's explicit signal.
        _pending          = q; // copy preserves correlation_id
        _pending.response = q.seq * 2;
        _asker            = q.getSource();
        _have_pending     = true;
    }
    void
    on(const ReleaseReply &) {
        if (_have_pending) {
            push<Ping>(_asker, _pending); // reply now — provably after the asker's timeout
            _have_pending = false;
        }
    }
};

class TraderLate : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderLate(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{5}, 40ms);
            } catch (const qb::io::async::timeout_error &) {
                g_ask_timed_out = true;
            }
            // We have provably timed out. Now tell the market to release its held reply; it will
            // arrive as an unsolicited Ping (the ask slot is gone) — the case under test.
            ctx.template push_to<ReleaseReply>(mkt);
        });
        co_return true;
    }
    void
    on(Ping &e) {
        if (resolve_ask(e))
            return; // would be a live ask; here the slot is gone → false
        g_late_unsolicited = true;
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAsk, LateReplyAfterTimeoutIsUnsolicitedAndSafe) {
    reset_flags();
    g_late_unsolicited = false;
    qb::Main main;
    auto     mkt = main.addActor<GatedMarket>(0);
    main.addActor<TraderLate>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load()) << "the ask must time out before the gated reply is released";
    EXPECT_TRUE(g_late_unsolicited.load()) << "a reply released after the timeout must arrive as unsolicited (resolve_ask false)";
}

// Cross-core variant: the timeout fires on core 0 while a reply is in flight from core 1. The slow
// market (core 1) replies after 80ms via a scoped coroutine; the asker times out at 40ms, then the
// in-flight reply lands as unsolicited. Exercises the cross-core teardown race.
namespace {
std::atomic<bool> g_xcore_late_unsolicited{false};
} // namespace

class TraderLateCrossCore : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TraderLateCrossCore(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{5}, 40ms); // SlowMarket replies at 80ms → timeout first
            } catch (const qb::io::async::timeout_error &) {
                g_ask_timed_out = true;
            }
            // Do NOT stop yet — wait for the in-flight cross-core reply to land as unsolicited.
        });
        co_return true;
    }
    void
    on(Ping &e) {
        if (resolve_ask(e))
            return; // slot gone after timeout → false
        g_xcore_late_unsolicited = true;
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAsk, TimeoutWhileReplyInFlightCrossCore) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to put the in-flight reply on a different core";
    }
    reset_flags();
    g_xcore_late_unsolicited = false;
    qb::Main main;
    // SlowMarket (core 1) answers seq*3 after 80ms via a scoped coroutine — the reply is mid-flight
    // when the asker's core-0 40ms timeout fires.
    auto mkt = main.addActor<SlowMarket>(1, 80ms);
    main.addActor<TraderLateCrossCore>(0, mkt); // asker on core 0
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_ask_timed_out.load()) << "the asker must time out (40ms) before the slow reply (80ms) arrives";
    EXPECT_TRUE(g_xcore_late_unsolicited.load())
        << "a cross-core reply arriving after the timeout must land as unsolicited, no double-resume/UAF";
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
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &e) {
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
        push<Ping>(_to, 1); // plain event, correlation_id stays 0
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
    EXPECT_TRUE(g_unsolicited_false.load()) << "resolve_ask must return false for an event with no pending ask";
}

// ---------------------------------------------------------------------------
// 8. ask round-trips a non-trivial (owning) payload correctly.
//    Uses a local Echo exchange (shared/AskResponders.h's Echoer answers a numeric
//    seq*2; this case needs an owning shared_ptr<string> payload).
// ---------------------------------------------------------------------------
struct Echo : public qb::AskEvent {
    std::shared_ptr<std::string> in;
    std::shared_ptr<std::string> out; // filled by responder
};
namespace {
std::atomic<bool> g_echo_ok{false};
} // namespace

class StringEchoer : public qb::Actor {
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
    auto     echoer = main.addActor<StringEchoer>(0);
    main.addActor<EchoClient>(0, echoer);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_echo_ok.load()) << "an owning shared_ptr<string> payload must round-trip intact";
}
