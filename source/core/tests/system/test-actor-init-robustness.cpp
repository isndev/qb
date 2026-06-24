/**
 * @file test-actor-init-robustness.cpp
 * @brief Robustness tests for async actor initialization and patterns-in-init.
 *
 * Complements test-actor-async-init{,-torture}: it focuses on the paths the first audit found
 * under-covered, with assertions that have teeth (they fail if the implementation is wrong),
 * and on running the high-level patterns library *inside* `onInit()` (the "patterns-in-init"
 * case), which stresses the activation dispatch gate end-to-end.
 *
 * Highlights:
 *   - Activation stash payload lifetime: a `push`'d event carrying a heap `std::string` that is
 *     stashed for an Activating actor must have its payload DESTROYED (not leaked) when the actor
 *     fails init / is killed / hits the deadline / overflows the stash cap, and exactly-once
 *     delivered+destroyed on the success path. Verified with a per-event ctor/dtor balance counter
 *     (this suite runs with ASAN detect_leaks=0, so LeakSanitizer would not catch the leak; the
 *     counter does — and ASan still catches any double-free).
 *   - `qb::ask` inside `onInit` that TIMES OUT (caught, init still completes).
 *   - patterns inside `onInit`: `ask_retry`, `ask_all`, and `run_saga` (with compensation).
 *   - coroutine-frame reclamation invariant on the in-init ask path (live_frames baseline).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/core/VirtualCore.h>
#include <qb/core/patterns.h>
#include <qb/io/async/coroutine.h>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace qb;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// A `push`'d event carrying a heap payload, with a strict construction/destruction
// balance counter. `live` returns to 0 iff every constructed instance is destroyed.
// The qb event layer byte-relocates events (no ctor/dtor on the relocation), so the
// only ctor is the placement-new at push and the only dtor is the framework disposer —
// `live != 0` after the engine drains means an event payload leaked.
// ---------------------------------------------------------------------------
struct PayloadEvent : public qb::Event {
    static std::atomic<long> live;
    std::string              data; // heap allocation (> SSO) so ASan also guards double-free
    int                      seq{0};

    PayloadEvent() {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    explicit PayloadEvent(int s)
        : data(64, 'x') // force a heap allocation
        , seq(s) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    PayloadEvent(const PayloadEvent &o)
        : qb::Event(o)
        , data(o.data)
        , seq(o.seq) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    PayloadEvent(PayloadEvent &&o) noexcept
        : qb::Event(o)
        , data(std::move(o.data))
        , seq(o.seq) {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    ~PayloadEvent() {
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};
std::atomic<long> PayloadEvent::live{0};

struct ScopedDeadline {
    std::uint64_t _saved;
    explicit ScopedDeadline(std::uint64_t ns)
        : _saved(qb::VirtualCore::activation_deadline_ns) {
        qb::VirtualCore::activation_deadline_ns = ns;
    }
    ~ScopedDeadline() {
        qb::VirtualCore::activation_deadline_ns = _saved;
    }
};

// ===========================================================================
// 1. Activation-stash payload lifetime — the leak fix (#2) regression suite.
// ===========================================================================

// --- 1a. onInit co_returns false after a co_await: stash must be DISPOSED, not leaked. ----
std::atomic<int> g_fail_handler_calls{0};

class FailsAfterAwait : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this); // registers the disposer for PayloadEvent
        co_await context().sleep(30ms);     // Activating while the burst piles up
        co_return false;                    // init fails → stash dropped (must be disposed)
    }
    void
    on(PayloadEvent &) {
        g_fail_handler_calls.fetch_add(1); // must NEVER run (actor failed init)
    }
};

class PayloadBurst : public qb::Actor {
    qb::ActorId _target;
    int         _n;

public:
    PayloadBurst(qb::ActorId t, int n)
        : _target(t)
        , _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 1; i <= _n; ++i)
            push<PayloadEvent>(_target, i);
        kill();
        co_return true;
    }
};

TEST(ActorInitRobustness, FailedAsyncInitDisposesStashedPayloads) {
    PayloadEvent::live.store(0);
    g_fail_handler_calls.store(0);
    {
        qb::Main   main;
        const auto victim = main.addActor<FailsAfterAwait>(0);
        main.addActor<PayloadBurst>(0, victim, 16);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_fail_handler_calls.load(), 0); // failed actor never handled a stashed event
    EXPECT_EQ(PayloadEvent::live.load(), 0L)   // every stashed payload was destroyed
        << "stashed events leaked when async init failed";
}

// --- 1b. Killed during init: KillEvent passes the gate; stash dropped + disposed. ----------
std::atomic<int> g_killed_handler_calls{0};

class ParksThenKilled : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().until_cancelled(); // parks until killed; throws cancelled_error
        co_return true;                       // unreachable
    }
    void
    on(PayloadEvent &) {
        g_killed_handler_calls.fetch_add(1);
    }
};

class BurstThenKill : public qb::Actor {
    qb::ActorId _target;
    int         _n;

public:
    BurstThenKill(qb::ActorId t, int n)
        : _target(t)
        , _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 1; i <= _n; ++i)
            push<PayloadEvent>(_target, i); // stashed (target Activating)
        push<qb::KillEvent>(_target);       // must pass the gate and unwind the in-flight init
        kill();
        co_return true;
    }
};

TEST(ActorInitRobustness, KillDuringInitPassesGateAndDisposesStash) {
    PayloadEvent::live.store(0);
    g_killed_handler_calls.store(0);
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksThenKilled>(0);
        main.addActor<BurstThenKill>(0, victim, 12);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_killed_handler_calls.load(), 0); // killed-during-init: stash never replayed
    EXPECT_EQ(PayloadEvent::live.load(), 0L) << "stashed payloads leaked when the actor was killed during init";
}

// --- 1c. Activation deadline expires: stash dropped + disposed. ----------------------------
class ParksForever : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().until_cancelled(); // never completes on its own
        co_return true;
    }
    void
    on(PayloadEvent &) {}
};

TEST(ActorInitRobustness, ActivationDeadlineDisposesStash) {
    PayloadEvent::live.store(0);
    ScopedDeadline dl(80ull * 1000ull * 1000ull); // 80ms deadline
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksForever>(0);
        main.addActor<PayloadBurst>(0, victim, 10);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(PayloadEvent::live.load(), 0L) << "stashed payloads leaked when the activation deadline expired";
}

// --- 1d. Stash overflow cap: the dropped overflow events are disposed too. ------------------
TEST(ActorInitRobustness, StashOverflowDisposesAllDroppedPayloads) {
    PayloadEvent::live.store(0);
    // Overflowing the cap itself forces the activation to fail (deadline set in the past),
    // so no ScopedDeadline is needed; the actor parks until that fail tears it down.
    constexpr int kBurst = 4200; // > kActivationStashCap (4096)
    {
        qb::Main   main;
        const auto victim = main.addActor<ParksForever>(0);
        main.addActor<PayloadBurst>(0, victim, kBurst);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(PayloadEvent::live.load(), 0L) << "overflowed/stashed payloads leaked when the stash cap was exceeded";
}

// --- 1e. Success path: stash replayed in FIFO order, each payload delivered then disposed. ---
std::atomic<int>  g_ok_count{0};
std::atomic<bool> g_ok_order{true};
std::atomic<bool> g_ok_all_after_active{true};

class ActivatesAndConsumes : public qb::Actor {
    int _next = 1;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PayloadEvent>(*this);
        co_await context().sleep(30ms);
        co_return true; // success → stash replayed
    }
    void
    on(PayloadEvent &e) {
        if (!is_active())
            g_ok_all_after_active.store(false); // replay must happen only once active
        if (e.seq != _next++)
            g_ok_order.store(false);
        if (g_ok_count.fetch_add(1) + 1 == 8)
            kill();
    }
};

TEST(ActorInitRobustness, SuccessReplaysFifoAndDisposesEachPayload) {
    PayloadEvent::live.store(0);
    g_ok_count.store(0);
    g_ok_order.store(true);
    g_ok_all_after_active.store(true);
    {
        qb::Main   main;
        const auto victim = main.addActor<ActivatesAndConsumes>(0);
        main.addActor<PayloadBurst>(0, victim, 8);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(g_ok_count.load(), 8);
    EXPECT_TRUE(g_ok_order.load());            // FIFO preserved across the activation boundary
    EXPECT_TRUE(g_ok_all_after_active.load()); // nothing delivered before activation
    EXPECT_EQ(PayloadEvent::live.load(), 0L);  // replayed events disposed exactly once
}

// ===========================================================================
// 2. qb::ask inside onInit that TIMES OUT (caught) — init still completes.
// ===========================================================================
struct Q : public qb::Request<int> {
    int key{0};
    Q() = default;
    explicit Q(int k)
        : key(k) {}
};

class SilentPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        co_return true;
    }
    void
    on(Q &) {} // never answers → asker times out
};

std::atomic<bool> g_ininit_timed_out{false};
std::atomic<bool> g_ininit_activated{false};

class AsksSilentInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksSilentInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        try {
            (void) co_await qb::ask(context(), _peer, Q{1}, 30ms);
        } catch (const qb::io::async::timeout_error &) {
            g_ininit_timed_out.store(true);
        }
        g_ininit_activated.store(true);
        qb::Main::stop(); // tear the engine down cleanly
        co_return true;   // a caught in-init timeout still activates the actor
    }
};

TEST(ActorInitRobustness, InInitAskTimeoutIsCaughtAndActivates) {
    g_ininit_timed_out.store(false);
    g_ininit_activated.store(false);
    qb::Main   main;
    const auto peer = main.addActor<SilentPeer>(0);
    main.addActor<AsksSilentInInit>(0, peer);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_ininit_timed_out.load());
    EXPECT_TRUE(g_ininit_activated.load());
}

// ===========================================================================
// 3. Patterns INSIDE onInit (patterns-in-init).
// ===========================================================================

// Answers on the Nth request, drops earlier ones (models transient failure for ask_retry).
class FlakyResponder : public qb::Actor {
    int _count{0};
    int _reply_on;

public:
    explicit FlakyResponder(int reply_on)
        : _reply_on(reply_on) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        co_return true;
    }
    void
    on(Q &q) {
        if (++_count >= _reply_on)
            qb::answer(*this, q, [](Q const &r) { return r.key * 10; });
    }
};

std::atomic<int> g_retry_in_init_value{-1};

class AskRetryInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskRetryInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        qb::retry_policy pol;
        pol.max_attempts = 5;
        pol.backoff      = 10ms;
        pol.max_backoff  = 40ms;
        auto r           = co_await qb::ask_retry(context(), _peer, Q{4}, 25ms, pol); // retries during init
        g_retry_in_init_value.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, AskRetryInsideOnInitSucceeds) {
    g_retry_in_init_value.store(-1);
    qb::Main   main;
    const auto flaky = main.addActor<FlakyResponder>(0, 2); // answers on the 2nd attempt
    main.addActor<AskRetryInInit>(0, flaky);
    main.start(false);
    main.join();
    EXPECT_EQ(g_retry_in_init_value.load(), 40); // 4 * 10, after a retry
}

// ask_all inside onInit — gather config from two peers while Activating.
class ConfigPeer : public qb::Actor {
    int _mult;

public:
    explicit ConfigPeer(int m)
        : _mult(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        co_return true;
    }
    void
    on(Q &q) {
        const int m = _mult;
        qb::answer(*this, q, [m](Q const &r) { return r.key * m; });
    }
};

std::atomic<int> g_askall_sum{-1};

class AskAllInInit : public qb::Actor {
    qb::ActorId _a, _b;

public:
    AskAllInInit(qb::ActorId a, qb::ActorId b)
        : _a(a)
        , _b(b) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        std::vector<qb::ActorId> peers{_a, _b};
        auto                     replies = co_await qb::ask_all(context(), peers, Q{3}, 500ms);
        int                      sum     = 0;
        for (auto const &r : replies)
            sum += r.response;
        g_askall_sum.store(sum);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, AskAllInsideOnInitGathers) {
    g_askall_sum.store(-1);
    qb::Main   main;
    const auto a = main.addActor<ConfigPeer>(0, 10);  // 3*10 = 30
    const auto b = main.addActor<ConfigPeer>(0, 100); // 3*100 = 300
    main.addActor<AskAllInInit>(0, a, b);
    main.start(false);
    main.join();
    EXPECT_EQ(g_askall_sum.load(), 330);
}

// run_saga inside onInit: a step fails → compensation runs (also an in-init ask) → init fails.
std::atomic<bool> g_saga_compensated{false};
std::atomic<int>  g_saga_compensate_calls{0};
std::atomic<bool> g_saga_init_failed{false};

class Reserver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        co_return true;
    }
    void
    on(Q &q) {
        qb::answer(*this, q, [](Q const &r) { return r.key; }); // always answers
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
        registerEvent<Q>(*this);
        auto reserver = _reserver;
        auto silent   = _silent;
        try {
            co_await qb::run_saga(context(), [reserver, silent](qb::ScopedCoroContext ctx, qb::SagaScope &saga) -> qb::io::async::task<void> {
                (void) co_await qb::ask(ctx, reserver, Q{1}, 500ms); // step 1 succeeds
                saga.on_compensate([ctx, reserver]() -> qb::io::async::task<void> {
                    (void) co_await qb::ask(ctx, reserver, Q{2}, 500ms); // undo (in-init ask)
                    g_saga_compensate_calls.fetch_add(1);
                    g_saga_compensated.store(true);
                });
                (void) co_await qb::ask(ctx, silent, Q{3}, 30ms); // step 2 TIMES OUT → rollback
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
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, SagaInsideOnInitCompensatesThenFailsInit) {
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
// 4. Coroutine-frame reclamation invariant on the in-init ask path.
//    start(false) runs the (single) core on THIS thread, so the worker's
//    thread_local live_frames is observable here.
// ===========================================================================
class EchoPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        co_return true;
    }
    void
    on(Q &q) {
        qb::answer(*this, q, [](Q const &r) { return r.key + 1; });
    }
};

std::atomic<int> g_frames_value{-1};

class AsksThenStops : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksThenStops(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        auto r = co_await qb::ask(context(), _peer, Q{41}, 500ms);
        g_frames_value.store(r.response);
        qb::Main::stop();
        co_return true;
    }
};

TEST(ActorInitRobustness, InInitAskReclaimsAllCoroutineFrames) {
    g_frames_value.store(-1);
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main   main;
        const auto peer = main.addActor<EchoPeer>(0);
        main.addActor<AsksThenStops>(0, peer);
        main.start(false); // last (only) core runs on this thread → live_frames is ours
        main.join();
    }
    EXPECT_EQ(g_frames_value.load(), 42);
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "in-init ask (onInit frame + ask awaiter + timer) leaked coroutine frames";
}

// ===========================================================================
// 4. NEW enrichment patterns INSIDE onInit (coverage extension).
//    ask-based patterns work while Activating because every per-target ask
//    registers its type so the reply bypasses the activation stash; scope-bound
//    helpers (rate_limiter / bulkhead) use ctx.sleep / the actor scope, which
//    exist during init. (ask_stream is the documented exception — see below.)
// ===========================================================================

// --- ask_quorum in init: k-of-N config gather while Activating ---
std::atomic<int> g_init_quorum_count{-1};

class AskQuorumInInit : public qb::Actor {
    std::vector<qb::ActorId> _peers;

public:
    explicit AskQuorumInInit(std::vector<qb::ActorId> peers)
        : _peers(std::move(peers)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        auto got = co_await qb::ask_quorum(context(), _peers, 2, Q{3}, 1s);
        g_init_quorum_count.store(static_cast<int>(got.size()));
        qb::Main::stop();
        co_return true;
    }
    void
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, AskQuorumInsideOnInit) {
    g_init_quorum_count.store(-1);
    qb::Main                 main;
    std::vector<qb::ActorId> peers;
    peers.push_back(main.addActor<ConfigPeer>(0, 10));
    peers.push_back(main.addActor<ConfigPeer>(0, 100));
    peers.push_back(main.addActor<ConfigPeer>(0, 1000));
    main.addActor<AskQuorumInInit>(0, peers);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_quorum_count.load(), 2); // first 2 of 3 reached during Activating
}

// --- ask_by (absolute deadline) in init ---
std::atomic<int> g_init_askby{-1};

class AskByInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskByInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        const auto dl = qb::deadline_in(context(), 1s);
        auto       r  = co_await qb::ask_by(context(), _peer, Q{5}, dl);
        g_init_askby.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, AskByDeadlineInsideOnInit) {
    g_init_askby.store(-1);
    qb::Main   main;
    const auto p = main.addActor<ConfigPeer>(0, 10); // 5 * 10 = 50
    main.addActor<AskByInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_askby.load(), 50);
}

// --- rate_limiter in init: 3rd acquire waits for a refill, all during onInit ---
std::atomic<int> g_init_rl{-1};

class RateLimiterInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto rl = std::make_shared<qb::rate_limiter>(2.0, 30ms); // 2 tokens, +1 per 30ms
        co_await rl->acquire(context());                         // token 1 (immediate)
        co_await rl->acquire(context());                         // token 2 (immediate)
        co_await rl->acquire(context());                         // waits ~30ms (refill) — scope-bound sleep
        g_init_rl.store(3);
        qb::Main::stop();
        co_return true;
    }
};

TEST(ActorInitRobustness, RateLimiterInsideOnInit) {
    g_init_rl.store(-1);
    qb::Main main;
    main.addActor<RateLimiterInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_rl.load(), 3); // all three acquired during init (3rd after a refill wait)
}

// --- bulkhead in init: hold a slot across an in-init ask ---
std::atomic<int> g_init_bh{-1};

class BulkheadInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit BulkheadInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Q>(*this);
        auto bh   = std::make_shared<qb::bulkhead>(1);
        auto slot = co_await bh->enter(context()); // admitted immediately (free)
        auto r    = co_await qb::ask(context(), _peer, Q{7}, 1s);
        g_init_bh.store(r.response);
        qb::Main::stop();
        co_return true;
    }
    void
    on(Q &e) {
        resolve_ask(e);
    }
};

TEST(ActorInitRobustness, BulkheadInsideOnInit) {
    g_init_bh.store(-1);
    qb::Main   main;
    const auto p = main.addActor<ConfigPeer>(0, 10); // 7 * 10 = 70
    main.addActor<BulkheadInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_bh.load(), 70);
}

// --- batcher in init: count trigger flushes synchronously while Activating ---
std::atomic<int> g_init_batch{-1};

class BatcherInInit : public qb::Actor {
    qb::batcher<int> _batch{3, 5s, [](std::vector<int> &&b) { g_init_batch.store(static_cast<int>(b.size())); }};

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

TEST(ActorInitRobustness, BatcherInsideOnInit) {
    g_init_batch.store(-1);
    qb::Main main;
    main.addActor<BatcherInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_batch.load(), 3); // flushed by count during init
}

// --- ask_stream in init: chunks now reach the Activating asker through the continuation registry
//     (the activation gate routes correlated replies), so a stream can be consumed DURING onInit. ---
struct SFeed : qb::StreamRequest<int> {
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

std::atomic<int> g_init_stream_n{-1};

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
        g_init_stream_n.store(n);
        qb::Main::stop();
        co_return true;
    }
    void
    on(SFeed &e) {
        (void) resolve_ask(e); // chunks are AskEvents → continuation registry
    }
};

TEST(ActorInitRobustness, AskStreamInsideOnInitDeliversChunks) {
    g_init_stream_n.store(-1);
    qb::Main   main;
    const auto p = main.addActor<StreamProd>(0);
    main.addActor<AskStreamInInit>(0, p);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_stream_n.load(), 3); // all chunks delivered while the asker was Activating
}

// --- require in init: discover-before-activate (a coordinator blocks activation until it knows its
//     peers). Replies reach the Activating coordinator via the continuation registry. ---
class InitWorker : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

std::atomic<int> g_init_require_count{-1};

class RequireInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // No on(RequireEvent) handler — Actor routes discovery replies by default.
        auto found = co_await qb::require<InitWorker>(context(), 150ms); // DIRECT co_await in onInit
        g_init_require_count.store(static_cast<int>(found.size()));
        qb::Main::stop();
        co_return true;
    }
};

TEST(ActorInitRobustness, RequireInsideOnInitDiscoversAll) {
    g_init_require_count.store(-1);
    qb::Main main;
    main.addActor<InitWorker>(0);
    main.addActor<InitWorker>(0);
    main.addActor<RequireInInit>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_init_require_count.load(), 2); // both workers discovered DURING onInit
}

} // namespace
