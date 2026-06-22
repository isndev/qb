/**
 * @file test-actor-patterns-enrich.cpp
 * @brief Tests for the patterns-library enrichments:
 *        - `qb::retry_policy::jitter` (+ `qb::detail::apply_retry_jitter` bounds),
 *        - bounded `qb::ask_all(..., max_in_flight)` (concurrency cap, correctness, order),
 *        - `qb::Supervisor` kill-propagation (no orphaned children) and sliding-window restart
 *          intensity,
 *        - `qb::run_saga` compensation that aborts cleanly when the actor is killed mid-rollback.
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
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace qb;
using namespace std::chrono_literals;

// ===========================================================================
// 1. retry_policy::jitter — pure unit tests on apply_retry_jitter bounds.
// ===========================================================================
TEST(RetryJitterUnit, ZeroJitterIsExact) {
    const qb::duration d = 100ms;
    for (int i = 0; i < 1000; ++i)
        EXPECT_EQ(qb::detail::apply_retry_jitter(d, 0.0), d);
}

TEST(RetryJitterUnit, HalfJitterStaysInLowerHalfBand) {
    const qb::duration d = 100ms;
    bool               saw_below_full = false;
    for (int i = 0; i < 5000; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 0.5);
        EXPECT_GE(r.count(), (d.count() / 2));     // >= d*(1-0.5)
        EXPECT_LE(r.count(), d.count());           // <= d
        if (r.count() < d.count())
            saw_below_full = true;
    }
    EXPECT_TRUE(saw_below_full); // jitter actually varies the value (not a constant)
}

TEST(RetryJitterUnit, FullJitterSpansZeroToD_AndClampsAboveOne) {
    const qb::duration d = 200ms;
    for (int i = 0; i < 5000; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 1.0);
        EXPECT_GE(r.count(), 0);
        EXPECT_LE(r.count(), d.count());
    }
    // jitter > 1 is clamped to 1 (no negative / out-of-band result).
    for (int i = 0; i < 1000; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 4.0);
        EXPECT_GE(r.count(), 0);
        EXPECT_LE(r.count(), d.count());
    }
}

// ===========================================================================
// 2. Bounded ask_all — concurrency cap vs unbounded.
// ===========================================================================
namespace {
std::atomic<int> g_inflight{0};   // asks received but not yet answered
std::atomic<int> g_max_inflight{0};
std::atomic<int> g_sum{0};

void
bump_max() {
    int cur = g_inflight.load();
    int prev = g_max_inflight.load();
    while (cur > prev && !g_max_inflight.compare_exchange_weak(prev, cur)) { /* retry */ }
}
} // namespace

struct Probe : public qb::Request<int> {
    int seq{0};
    Probe() = default;
    explicit Probe(int s)
        : seq(s) {}
    Probe(int s, std::uint64_t corr, int resp)
        : seq(s) {
        this->correlation_id = corr;
        this->response       = resp;
    }
};

// Holds each request ~15ms before replying, so concurrently-outstanding asks are observable.
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
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

static void
reset_scatter() {
    g_inflight.store(0);
    g_max_inflight.store(0);
    g_sum.store(0);
}

TEST(BoundedScatter, CapLimitsConcurrencyAndKeepsResults) {
    reset_scatter();
    constexpr int N   = 6;
    constexpr int CAP = 2;
    qb::Main      main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < N; ++i)
        targets.push_back(main.addActor<SlowResponder>(0));
    main.addActor<BoundedAsker>(0, targets, CAP);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_LE(g_max_inflight.load(), CAP); // never more than CAP outstanding — the cap holds
    EXPECT_GE(g_max_inflight.load(), 1);
    EXPECT_EQ(g_sum.load(), N * 11); // each response = seq(10)+1 = 11
}

TEST(BoundedScatter, UnboundedRunsAllAtOnce) {
    reset_scatter();
    constexpr int N = 6;
    qb::Main      main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < N; ++i)
        targets.push_back(main.addActor<SlowResponder>(0));
    main.addActor<BoundedAsker>(0, targets, 0); // 0 => unbounded
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_GT(g_max_inflight.load(), 2); // unbounded fans out beyond the bounded cap (contrast)
    EXPECT_EQ(g_sum.load(), N * 11);
}

// ===========================================================================
// 3. Supervisor kill-propagation — killing the supervisor tears down children.
// ===========================================================================
namespace {
std::atomic<int> g_kp_alive{0};      // live KpWorker instances (onInit ++, dtor --)
std::atomic<int> g_kp_alive_check{0}; // snapshot taken after the supervisor is killed
} // namespace

struct KpKill : public qb::Event {}; // test-control: tells the supervisor to receive a KillEvent

class KpWorker : public qb::SupervisedActor {
public:
    KpWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen)
        : qb::SupervisedActor(sup, slot, gen) {}
    qb::io::async::task<bool>
    onInit() override {
        g_kp_alive.fetch_add(1);
        co_return true;
    }
    ~KpWorker() override {
        g_kp_alive.fetch_sub(1);
    }
};

class KpSupervisor : public qb::Supervisor {
public:
    KpSupervisor()
        : qb::Supervisor(qb::restart_strategy::one_for_one, 3) {}

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        return addRefActor<KpWorker>(id(), slot, generation).id();
    }
};

class KpDriver : public qb::Actor {
    qb::ActorId _sup;

public:
    explicit KpDriver(qb::ActorId sup)
        : _sup(sup) {}
    qb::io::async::task<bool>
    onInit() override {
        // Kill ONLY the supervisor (a targeted KillEvent), then snapshot child liveness, then
        // stop the engine. With kill-propagation the children are already gone at the snapshot.
        push<qb::KillEvent>(_sup);                                            // kill supervisor now
        qb::io::async::callback([] { g_kp_alive_check.store(g_kp_alive.load()); }, 50ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 120ms);
        co_return true;
    }
};

TEST(SupervisorKillPropagation, KillingSupervisorKillsChildren) {
    g_kp_alive.store(0);
    g_kp_alive_check.store(-1);
    qb::Main   main;
    const auto sup = main.addActor<KpSupervisor>(0);
    main.addActor<KpDriver>(0, sup);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_kp_alive_check.load(), 0); // children torn down with the supervisor (no orphans)
    EXPECT_EQ(g_kp_alive.load(), 0);       // and nothing leaked at shutdown
}

// ===========================================================================
// 4. Supervisor sliding-window restart intensity.
// ===========================================================================
namespace {
std::atomic<int>  g_win_spawns{0};
std::atomic<bool> g_win_escalated{false};
} // namespace

struct WinCrash : public qb::Event {};
struct WinTrigger : public qb::Event {};

class WinWorker : public qb::SupervisedActor {
public:
    WinWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen)
        : qb::SupervisedActor(sup, slot, gen) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WinCrash>(*this);
        g_win_spawns.fetch_add(1);
        co_return true;
    }
    void
    on(WinCrash &) {
        stop();
    }
};

class WinSupervisor : public qb::Supervisor {
public:
    WinSupervisor()
        // max 3 restarts within a 10s window (huge vs the test) → 4th ChildDown escalates.
        : qb::Supervisor(qb::restart_strategy::one_for_one, 1, 3, 10s) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WinTrigger>(*this);
        co_return co_await qb::Supervisor::onInit();
    }
    void
    on(WinTrigger &) {
        push<WinCrash>(child(0));
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        return addRefActor<WinWorker>(id(), slot, generation).id();
    }
    void
    on_escalate() override {
        g_win_escalated.store(true);
    }
};

class WinDriver : public qb::Actor {
    qb::ActorId _sup;

public:
    explicit WinDriver(qb::ActorId sup)
        : _sup(sup) {}
    qb::io::async::task<bool>
    onInit() override {
        auto sup = _sup;
        for (int i = 0; i < 4; ++i) // 4 crashes → 3 restarts + 1 escalation
            qb::io::async::callback([this, sup] { push<WinTrigger>(sup); }, std::chrono::milliseconds{20 + i * 25});
        qb::io::async::callback([] { qb::Main::stop(); }, 220ms);
        co_return true;
    }
};

TEST(SupervisorRestartWindow, EscalatesWithinWindow) {
    g_win_spawns.store(0);
    g_win_escalated.store(false);
    qb::Main   main;
    const auto sup = main.addActor<WinSupervisor>(0);
    main.addActor<WinDriver>(0, sup);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_win_escalated.load());      // 4th restart within the window escalated
    EXPECT_EQ(g_win_spawns.load(), 1 + 3);    // 1 initial + 3 restarts (4th escalated, no respawn)
}

// ===========================================================================
// 5. Saga compensation aborts cleanly when killed mid-rollback.
// ===========================================================================
namespace {
std::atomic<bool> g_compA_entered{false};  // compA's body — must NOT even be entered (break skips it)
std::atomic<bool> g_compA_ran{false};      // compA completed — must be false on kill
std::atomic<bool> g_compB_started{false};  // runs FIRST in rollback — reached, then cancelled
std::atomic<bool> g_saga_failed{false};
} // namespace

struct SagaQ : public qb::Request<int> {
    int v{0};
    SagaQ() = default;
    explicit SagaQ(int x)
        : v(x) {}
};

class OkPeer : public qb::Actor { // answers immediately
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        co_return true;
    }
    void
    on(SagaQ &q) {
        qb::answer(*this, q, [](SagaQ const &r) { return r.v; });
    }
};

class SilentPeer2 : public qb::Actor { // never answers
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        co_return true;
    }
    void
    on(SagaQ &) {}
};

class SagaActor : public qb::Actor {
    qb::ActorId _ok;
    qb::ActorId _silent;

public:
    SagaActor(qb::ActorId ok, qb::ActorId silent)
        : _ok(ok)
        , _silent(silent) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        auto ok     = _ok;
        auto silent = _silent;
        spawn([ok, silent](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(
                    ctx, [ok, silent](qb::ScopedCoroContext c, qb::SagaScope &saga) -> qb::io::async::task<void> {
                        (void) co_await qb::ask(c, ok, SagaQ{1}, 500ms); // step 1 ok
                        saga.on_compensate([c, ok]() -> qb::io::async::task<void> {
                            g_compA_entered.store(true);                     // compA body entered?
                            (void) co_await qb::ask(c, ok, SagaQ{2}, 500ms); // compA — runs LAST
                            g_compA_ran.store(true);
                        });
                        (void) co_await qb::ask(c, ok, SagaQ{3}, 500ms); // step 2 ok
                        saga.on_compensate([c, silent]() -> qb::io::async::task<void> {
                            g_compB_started.store(true);                 // compB — runs FIRST
                            (void) co_await qb::ask(c, silent, SagaQ{4}, 500ms); // parks (silent peer)
                        });
                        (void) co_await qb::ask(c, silent, SagaQ{5}, 30ms); // step 3 TIMES OUT → rollback
                    });
            } catch (const qb::io::async::timeout_error &) {
                g_saga_failed.store(true); // the saga failed (and rolled back as far as it could)
            } catch (const qb::io::async::cancelled_error &) {
                // possible if the kill races the rethrow — also acceptable
            }
        });
        co_return true;
    }
    void
    on(SagaQ &e) {
        resolve_ask(e); // route our own asks' replies back to the saga coroutine
    }
};

class SagaKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit SagaKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto victim = _victim;
        // Compensation begins ~30ms (after step-3 timeout); compB then parks on the silent peer.
        // Kill at 70ms → compB's ask is cancelled → rollback aborts before compA.
        qb::io::async::callback([this, victim] { push<qb::KillEvent>(victim); }, 70ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 200ms);
        co_return true;
    }
};

TEST(SagaCancel, CompensationAbortsOnKillMidRollback) {
    g_compA_entered.store(false);
    g_compA_ran.store(false);
    g_compB_started.store(false);
    g_saga_failed.store(false);
    qb::Main   main;
    const auto ok     = main.addActor<OkPeer>(0);
    const auto silent = main.addActor<SilentPeer2>(0);
    const auto saga   = main.addActor<SagaActor>(0, ok, silent);
    main.addActor<SagaKiller>(0, saga);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_compB_started.load());   // rollback reached the first compensation
    EXPECT_FALSE(g_compA_entered.load());  // …and aborted there: compA's body was never entered
    EXPECT_FALSE(g_compA_ran.load());      // (so compA certainly did not complete either)
}

// ===========================================================================
// 6. ask_quorum — first K-of-N replies.
// ===========================================================================
namespace {
std::atomic<int>  g_q_size{-1};
std::atomic<int>  g_q_sum{0};
std::atomic<bool> g_q_timeout{false};
std::atomic<bool> g_q_cancelled{false};
} // namespace

class FastResponder : public qb::Actor { // answers immediately (response = seq + 1)
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe &p) {
        qb::answer(*this, p, [](Probe const &r) { return r.seq + 1; });
    }
};

class SilentResponder : public qb::Actor { // never answers
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe &) {}
};

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
            if (stop)
                qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

static void
reset_quorum() {
    g_q_size.store(-1);
    g_q_sum.store(0);
    g_q_timeout.store(false);
    g_q_cancelled.store(false);
}

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
    EXPECT_EQ(g_q_size.load(), 3);     // exactly k results
    EXPECT_EQ(g_q_sum.load(), 3 * 11); // each response = seq(10)+1
    EXPECT_FALSE(g_q_timeout.load());
    EXPECT_FALSE(g_q_cancelled.load());
}

TEST(AskQuorum, UnreachableThrowsTimeout) {
    reset_quorum();
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    targets.push_back(main.addActor<FastResponder>(0));  // 2 can answer …
    targets.push_back(main.addActor<FastResponder>(0));
    targets.push_back(main.addActor<SilentResponder>(0)); // … 3 never do → quorum of 3 impossible
    targets.push_back(main.addActor<SilentResponder>(0));
    targets.push_back(main.addActor<SilentResponder>(0));
    main.addActor<QuorumAsker>(0, targets, /*k*/ 3, 40ms, /*stop*/ true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_timeout.load()); // 3 failures > N-k (2) → unreachable → timeout_error
    EXPECT_EQ(g_q_size.load(), -1);  // no result delivered
}

class QuorumKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit QuorumKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill while parked
        qb::io::async::callback([] { qb::Main::stop(); }, 150ms);
        co_return true;
    }
};

TEST(AskQuorum, CancelledOnKill) {
    reset_quorum();
    qb::Main                 main;
    std::vector<qb::ActorId> targets;
    for (int i = 0; i < 3; ++i)
        targets.push_back(main.addActor<SilentResponder>(0)); // never answer (long timeout) → parked
    // stop=false: the asker must NOT stop the engine; the killer drives shutdown.
    const auto asker = main.addActor<QuorumAsker>(0, targets, /*k*/ 2, 5s, /*stop*/ false);
    main.addActor<QuorumKiller>(0, asker);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_q_cancelled.load()); // killed while waiting → cancelled_error
    EXPECT_FALSE(g_q_timeout.load());
}

TEST(AskQuorum, ReclaimsAllCoroutineFrames) {
    reset_quorum();
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main                 main;
        std::vector<qb::ActorId> targets;
        for (int i = 0; i < 5; ++i)
            targets.push_back(main.addActor<FastResponder>(0));
        main.addActor<QuorumAsker>(0, targets, /*k*/ 3, 500ms, /*stop*/ true);
        main.start(false); // single core → this thread is the worker; live_frames is ours
        main.join();
    }
    EXPECT_EQ(g_q_size.load(), 3);
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "ask_quorum leaked coroutine frames (quorum awaiter + N collectors)";
}

// ===========================================================================
// 7. rate_limiter (token bucket).
// ===========================================================================
TEST(RateLimiterUnit, RefillsOverTimeAndCaps) {
    constexpr std::uint64_t MS = 1'000'000ull;
    qb::rate_limiter        rl(2.0, 10ms); // capacity 2, one token / 10ms

    EXPECT_TRUE(rl.try_acquire(0));         // starts full (2)
    EXPECT_TRUE(rl.try_acquire(0));         // …2nd token
    EXPECT_FALSE(rl.try_acquire(0));        // empty
    EXPECT_FALSE(rl.try_acquire(5 * MS));   // only 0.5 token regenerated
    EXPECT_TRUE(rl.try_acquire(10 * MS));   // 1 full token at 10ms
    EXPECT_FALSE(rl.try_acquire(10 * MS));  // …consumed

    // After a long idle the bucket caps at `capacity` (2), not unbounded accumulation.
    EXPECT_TRUE(rl.try_acquire(1000 * MS));
    EXPECT_TRUE(rl.try_acquire(1000 * MS));
    EXPECT_FALSE(rl.try_acquire(1000 * MS));
}

namespace {
std::atomic<int> g_rl_acquired{0};
} // namespace

class RateLimitedActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto rl = std::make_shared<qb::rate_limiter>(2.0, 15ms); // burst 2, then 1/15ms
            for (int i = 0; i < 4; ++i) {
                co_await rl->acquire(c); // first 2 immediate, next 2 wait (cancellation-aware)
                g_rl_acquired.fetch_add(1);
            }
            qb::Main::stop();
        });
        co_return true;
    }
};

TEST(RateLimiter, AcquireThrottlesAndCompletes) {
    g_rl_acquired.store(0);
    qb::Main main;
    main.addActor<RateLimitedActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_rl_acquired.load(), 4); // all four acquired (throttled, not dropped)
}

// ===========================================================================
// 8. deadline / ask_by — absolute budget propagation.
// ===========================================================================
namespace {
std::atomic<long> g_dl_remaining_ns{-1};
std::atomic<bool> g_dl_past_timeout{false};
std::atomic<int>  g_dl_val{-1};
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
            qb::Main::stop();
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
    qb::Main   main;
    const auto fast = main.addActor<FastResponder>(0);
    main.addActor<DeadlineAsker>(0, fast);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_dl_remaining_ns.load(), 100'000'000L); // exact within one cached-time loop pass
    EXPECT_TRUE(g_dl_past_timeout.load());             // already-spent budget fails fast
    EXPECT_EQ(g_dl_val.load(), 6);                     // future deadline → reply (seq 5 + 1)
}

// ===========================================================================
// 9. bulkhead — bounds concurrent operations, cancel-aware.
// ===========================================================================
namespace {
std::atomic<int>  g_bh_inflight{0};
std::atomic<int>  g_bh_max{0};
std::atomic<int>  g_bh_done{0};
std::atomic<bool> g_bh_cancelled{false};

void
bh_bump_max() {
    int cur = g_bh_inflight.load(), prev = g_bh_max.load();
    while (cur > prev && !g_bh_max.compare_exchange_weak(prev, cur)) { /* retry */ }
}
} // namespace

class BulkheadActor : public qb::Actor {
    std::shared_ptr<qb::bulkhead> _bh = std::make_shared<qb::bulkhead>(2); // at most 2 concurrent

public:
    qb::io::async::task<bool>
    onInit() override {
        auto bh = _bh;
        for (int i = 0; i < 5; ++i) {
            spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
                auto slot = co_await bh->enter(c); // waits while 2 are already in flight
                g_bh_inflight.fetch_add(1);
                bh_bump_max();
                co_await c.sleep(15ms);
                g_bh_inflight.fetch_sub(1);
                if (g_bh_done.fetch_add(1) + 1 == 5)
                    qb::Main::stop();
            });
        }
        co_return true;
    }
};

TEST(Bulkhead, CapsConcurrency) {
    g_bh_inflight.store(0);
    g_bh_max.store(0);
    g_bh_done.store(0);
    qb::Main main;
    main.addActor<BulkheadActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bh_done.load(), 5);   // all five ran
    EXPECT_LE(g_bh_max.load(), 2);    // …but never more than 2 at once — the bulkhead holds
    EXPECT_GE(g_bh_max.load(), 1);
}

class BulkheadCancelActor : public qb::Actor {
    std::shared_ptr<qb::bulkhead> _bh = std::make_shared<qb::bulkhead>(1); // single slot

public:
    qb::io::async::task<bool>
    onInit() override {
        auto bh = _bh;
        // Holder takes the only slot and keeps it for a long time.
        spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto slot = co_await bh->enter(c);
            co_await c.sleep(5s);
        });
        // Waiter parks on a full bulkhead → must be cancelled on kill (not hang).
        spawn([bh](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                auto slot = co_await bh->enter(c);
            } catch (const qb::io::async::cancelled_error &) {
                g_bh_cancelled.store(true);
            }
        });
        co_return true;
    }
};

class BulkheadKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit BulkheadKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 150ms);
        co_return true;
    }
};

TEST(Bulkhead, ParkedEnterCancelledOnKill) {
    g_bh_cancelled.store(false);
    qb::Main   main;
    const auto victim = main.addActor<BulkheadCancelActor>(0);
    main.addActor<BulkheadKiller>(0, victim);
    main.start(false);
    main.join(); // must NOT hang — the cancel-aware enter retracts the parked waiter
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_bh_cancelled.load());
}

// ===========================================================================
// 10. idempotency — responder de-dup by stable key (exactly-once effects).
// ===========================================================================
TEST(Idempotency, DedupMapIsLRU) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    m.put(2, 20);
    EXPECT_EQ(m.size(), 2u);
    ASSERT_NE(m.find(1), nullptr); // touch 1 → 1 becomes MRU, 2 is now LRU
    EXPECT_EQ(*m.find(1), 10);
    m.put(3, 30);                  // over capacity → evict LRU (2)
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.find(2), nullptr); // 2 evicted
    ASSERT_NE(m.find(1), nullptr); // 1 survived (was touched)
    ASSERT_NE(m.find(3), nullptr);
    EXPECT_EQ(*m.find(3), 30);
}

namespace {
std::atomic<int> g_idem_processed{0}; // counts responder effect executions
std::atomic<int> g_idem_r1{-1};
std::atomic<int> g_idem_r2{-1};

struct Charge : qb::Request<int> {
    std::uint64_t idempotency_key{0};
    int           amount{0};
};
} // namespace

class IdemBank : public qb::Actor {
    qb::dedup_map<std::uint64_t, int> _seen{16};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        co_return true;
    }
    void
    on(Charge &e) {
        qb::answer_idempotent(*this, e, _seen, [](Charge const &r) {
            g_idem_processed.fetch_add(1); // the effect — must run at most once per non-zero key
            return r.amount * 10;
        });
    }
};

class IdemAsker : public qb::Actor {
    qb::ActorId   _bank;
    std::uint64_t _k1, _k2;
    int           _a1, _a2;

public:
    IdemAsker(qb::ActorId bank, std::uint64_t k1, int a1, std::uint64_t k2, int a2)
        : _bank(bank)
        , _k1(k1)
        , _k2(k2)
        , _a1(a1)
        , _a2(a2) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        auto bank = _bank;
        auto k1 = _k1, k2 = _k2;
        auto a1 = _a1, a2 = _a2;
        spawn([bank, k1, k2, a1, a2](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Charge c1;
            c1.idempotency_key = k1;
            c1.amount          = a1;
            auto r1            = co_await qb::ask(c, bank, c1, 1s);
            g_idem_r1.store(r1.response);
            Charge c2;
            c2.idempotency_key = k2;
            c2.amount          = a2;
            auto r2            = co_await qb::ask(c, bank, c2, 1s);
            g_idem_r2.store(r2.response);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Charge &e) {
        resolve_ask(e);
    }
};

static void
run_idem(std::uint64_t k1, int a1, std::uint64_t k2, int a2) {
    g_idem_processed.store(0);
    g_idem_r1.store(-1);
    g_idem_r2.store(-1);
    qb::Main   main;
    const auto bank = main.addActor<IdemBank>(0);
    main.addActor<IdemAsker>(0, bank, k1, a1, k2, a2);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(Idempotency, SameKeyProcessedOnce) {
    run_idem(42, 5, 42, 5);                // same key twice (a retry / duplicate)
    EXPECT_EQ(g_idem_r1.load(), 50);
    EXPECT_EQ(g_idem_r2.load(), 50);       // second reply is the cached response
    EXPECT_EQ(g_idem_processed.load(), 1); // …but the effect ran exactly once
}

TEST(Idempotency, ZeroKeyNotCached) {
    run_idem(0, 5, 0, 5); // default key → never de-duplicated
    EXPECT_EQ(g_idem_r1.load(), 50);
    EXPECT_EQ(g_idem_r2.load(), 50);
    EXPECT_EQ(g_idem_processed.load(), 2); // effect ran each time
}

TEST(Idempotency, DistinctKeysBothProcessed) {
    run_idem(1, 1, 2, 2); // different keys → independent
    EXPECT_EQ(g_idem_r1.load(), 10);
    EXPECT_EQ(g_idem_r2.load(), 20);
    EXPECT_EQ(g_idem_processed.load(), 2);
}

// ===========================================================================
// 11. batcher — size/time-windowed aggregation, scope-bound window timer.
// ===========================================================================
namespace {
std::atomic<int> g_batch_flushes{0};
std::atomic<int> g_batch_items{0};
std::atomic<int> g_batch_pending{-1};
} // namespace

// Adds `count` ints, then stops after `stop_after` (window long enough not to fire unless tested).
class BatchCountActor : public qb::Actor {
    qb::batcher<int> _batch{3, 5s, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 7; ++i)
            _batch.add(context(), i); // flushes synchronously at 3 and at 6
        g_batch_pending.store(static_cast<int>(_batch.pending()));
        qb::io::async::callback([] { qb::Main::stop(); }, 50ms);
        co_return true;
    }
};

TEST(Batcher, FlushesOnCount) {
    g_batch_flushes.store(0);
    g_batch_items.store(0);
    g_batch_pending.store(-1);
    qb::Main main;
    main.addActor<BatchCountActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_flushes.load(), 2); // 7 items, max 3 → flush at 3 and 6
    EXPECT_EQ(g_batch_items.load(), 6);
    EXPECT_EQ(g_batch_pending.load(), 1); // item #7 still buffered (window 5s did not fire)
}

class BatchWindowActor : public qb::Actor {
    qb::batcher<int> _batch{100, 30ms, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // arms the 30ms window timer
        _batch.add(context(), 2); // same batch, count (2) below max (100)
        qb::io::async::callback([] { qb::Main::stop(); }, 120ms);
        co_return true;
    }
};

TEST(Batcher, FlushesOnWindow) {
    g_batch_flushes.store(0);
    g_batch_items.store(0);
    qb::Main main;
    main.addActor<BatchWindowActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_flushes.load(), 1); // count never hit → the time window flushed
    EXPECT_EQ(g_batch_items.load(), 2);
}

// Buffers below max with a long window, then is killed before the window fires.
class BatchKillActor : public qb::Actor {
    qb::batcher<int> _batch{100, 5s, [](std::vector<int> &&b) {
                                g_batch_flushes.fetch_add(1);
                                g_batch_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // arms a 5s scope-bound timer
        _batch.add(context(), 2);
        g_batch_pending.store(static_cast<int>(_batch.pending())); // proof: 2 items really buffered
        // Self-kill before the window: the actor dies → engine empties → stops. No lingering
        // stop-callback (one that outlived an early-stopping engine would fire into the next test).
        qb::io::async::callback([this] { if (is_alive()) kill(); }, 40ms);
        co_return true;
    }
};

TEST(Batcher, KillCancelsPendingFlush) {
    g_batch_flushes.store(0);
    g_batch_items.store(0);
    g_batch_pending.store(-1);
    qb::Main main;
    main.addActor<BatchKillActor>(0);
    main.start(false);
    main.join(); // must not hang; the scope-bound window timer is cancelled on kill
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_batch_pending.load(), 2); // …items were buffered (teeth: not a no-op)
    EXPECT_EQ(g_batch_flushes.load(), 0); // …then killed before the window → dropped, not flushed
}

// ===========================================================================
// 12. ask_stream — multi-reply streaming over the mailbox.
// ===========================================================================
namespace {
std::atomic<int>  g_stream_n{0};
std::atomic<int>  g_stream_sum{0};
std::atomic<bool> g_stream_timeout{false};
std::atomic<bool> g_stream_cancelled{false};

struct Feed : qb::StreamRequest<int> {
    int count{0};
};
} // namespace

class StreamProducer : public qb::Actor {
    bool _end;

public:
    explicit StreamProducer(bool end = true)
        : _end(end) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        co_return true;
    }
    void
    on(Feed &e) {
        for (int i = 0; i < e.count; ++i)
            qb::yield_answer(*this, e, i * 10); // chunks 0,10,20,…
        if (_end)
            qb::end_stream(*this, e);
    }
};

class StreamConsumer : public qb::Actor {
    qb::ActorId  _prod;
    int          _count;
    qb::duration _to;

public:
    StreamConsumer(qb::ActorId prod, int count, qb::duration to)
        : _prod(prod)
        , _count(count)
        , _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        auto prod  = _prod;
        auto count = _count;
        auto to    = _to;
        spawn([prod, count, to](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Feed f;
            f.count = count;
            auto s  = qb::ask_stream(c, prod, f, to);
            int  n = 0, sum = 0;
            try {
                while (auto chunk = co_await s.next()) {
                    ++n;
                    sum += chunk->chunk;
                }
            } catch (const qb::io::async::timeout_error &) {
                g_stream_timeout.store(true);
            } catch (const qb::io::async::cancelled_error &) {
                g_stream_cancelled.store(true);
            }
            g_stream_n.store(n);
            g_stream_sum.store(sum);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Feed &e) {
        (void) resolve_ask(e); // chunks are AskEvents → routed via the continuation registry
    }
};

TEST(AskStream, StreamsAllChunksInOrder) {
    g_stream_n.store(0);
    g_stream_sum.store(0);
    g_stream_timeout.store(false);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/true);
    main.addActor<StreamConsumer>(0, prod, 5, qb::duration{1s});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_stream_n.load(), 5);     // all five chunks drained
    EXPECT_EQ(g_stream_sum.load(), 100); // 0+10+20+30+40 — FIFO order preserved
    EXPECT_FALSE(g_stream_timeout.load());
}

TEST(AskStream, TimeoutBetweenChunks) {
    g_stream_n.store(0);
    g_stream_sum.store(0);
    g_stream_timeout.store(false);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/false); // emits, never ends
    main.addActor<StreamConsumer>(0, prod, 1, qb::duration{100ms});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_stream_n.load(), 1);       // got the one chunk
    EXPECT_TRUE(g_stream_timeout.load());  // …then next() timed out (no end marker)
}

class StreamKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit StreamKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 200ms);
        co_return true;
    }
};

TEST(AskStream, CancelledOnKill) {
    g_stream_cancelled.store(false);
    qb::Main   main;
    const auto prod   = main.addActor<StreamProducer>(0, /*end=*/false);
    const auto victim = main.addActor<StreamConsumer>(0, prod, 1, qb::duration{5s});
    main.addActor<StreamKiller>(0, victim);
    main.start(false);
    main.join(); // must not hang — kill wakes the parked next(), which throws cancelled
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_stream_cancelled.load());
}

// ===========================================================================
// 13. discovery — coroutine qb::ping (liveness) + qb::require (typed discovery).
// ===========================================================================
namespace {
std::atomic<int>  g_disc_alive{-1};   // ping known target
std::atomic<int>  g_disc_dead{-1};    // ping invalid target
std::atomic<int>  g_disc_count{-1};   // require<DiscWorker> count
std::atomic<bool> g_disc_cancelled{false};
} // namespace

// Plain actor: the kernel auto-registers PingEvent, so it answers discovery/liveness out of the box.
class DiscWorker : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

class DiscProbe : public qb::Actor {
    qb::ActorId _target;

public:
    explicit DiscProbe(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        // No registerEvent<RequireEvent> / on(RequireEvent) — Actor routes discovery replies by default.
        auto target = _target;
        spawn([target](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            g_disc_alive.store(co_await qb::ping(c, target, 150ms) ? 1 : 0);          // alive
            g_disc_dead.store(co_await qb::ping(c, qb::ActorId{}, 100ms) ? 1 : 0);    // invalid → timeout
            auto found = co_await qb::require<DiscWorker>(c, 120ms);                   // discover all
            g_disc_count.store(static_cast<int>(found.size()));
            qb::Main::stop();
        });
        co_return true;
    }
};

TEST(Discovery, PingAndRequire) {
    qb::io::async::listener::current.clear(); // drop any leftover timer from a prior test's killer
    g_disc_alive.store(-1);
    g_disc_dead.store(-1);
    g_disc_count.store(-1);
    qb::Main   main;
    const auto w0 = main.addActor<DiscWorker>(0);
    main.addActor<DiscWorker>(0);
    main.addActor<DiscWorker>(0);
    main.addActor<DiscProbe>(0, w0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_disc_alive.load(), 1);  // known worker replied
    EXPECT_EQ(g_disc_dead.load(), 0);   // invalid id → timed out
    EXPECT_EQ(g_disc_count.load(), 3);  // all three DiscWorkers discovered
}

class DiscProbeCancel : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                co_await qb::ping(c, qb::ActorId{}, 5s); // never answers; long wait
            } catch (const qb::io::async::cancelled_error &) {
                g_disc_cancelled.store(true);
            }
        });
        co_return true;
    }
};

class DiscKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit DiscKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 150ms);
        co_return true;
    }
};

TEST(Discovery, PingCancelledOnKill) {
    qb::io::async::listener::current.clear(); // drop any leftover timer from a prior test's killer
    g_disc_cancelled.store(false);
    qb::Main   main;
    const auto victim = main.addActor<DiscProbeCancel>(0);
    main.addActor<DiscKiller>(0, victim);
    main.start(false);
    main.join(); // must not hang — kill wakes the parked ping, which throws cancelled
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_disc_cancelled.load());
}

// ===========================================================================
// 14. Coverage backfill — documented behaviours that lacked a teeth-bearing test.
// ===========================================================================

// --- bulkhead: the non-blocking try_enter() + available() accounting (pure, no engine). ---
TEST(BulkheadUnit, TryEnterAndAvailable) {
    qb::bulkhead          bh(2);
    qb::bulkhead::slot    s1, s2, s3;
    EXPECT_EQ(bh.available(), 2u);
    EXPECT_TRUE(bh.try_enter(s1));
    EXPECT_EQ(bh.available(), 1u);
    EXPECT_TRUE(bh.try_enter(s2));
    EXPECT_EQ(bh.available(), 0u);
    EXPECT_FALSE(bh.try_enter(s3)); // full → refused, no slot handed out
    s1.release();                   // free one permit early
    EXPECT_EQ(bh.available(), 1u);
    EXPECT_TRUE(bh.try_enter(s3));   // now admits
    EXPECT_EQ(bh.available(), 0u);
}

// --- dedup_map: put-update (same key), contains (no promote), clear, capacity clamp (pure). ---
TEST(Idempotency, DedupMapPutUpdateContainsClear) {
    qb::dedup_map<int, int> m(2);
    m.put(1, 10);
    EXPECT_TRUE(m.contains(1));
    EXPECT_FALSE(m.contains(9));
    m.put(1, 11); // same key → update value in place, no growth
    EXPECT_EQ(m.size(), 1u);
    ASSERT_NE(m.find(1), nullptr);
    EXPECT_EQ(*m.find(1), 11);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.contains(1));

    qb::dedup_map<int, int> z(0); // capacity clamps to >= 1
    EXPECT_EQ(z.capacity(), 1u);
    z.put(1, 1);
    z.put(2, 2);                  // evicts 1 (cap 1)
    EXPECT_EQ(z.size(), 1u);
    EXPECT_FALSE(z.contains(1));
    EXPECT_TRUE(z.contains(2));
}

// --- ask_quorum edge cases: empty/zero-k → empty vector; k>n clamps to n. ---
TEST(AskQuorum, ZeroKReturnsEmpty) {
    reset_quorum();
    qb::Main   main;
    const auto a = main.addActor<FastResponder>(0);
    const auto b = main.addActor<FastResponder>(0);
    main.addActor<QuorumAsker>(0, std::vector<qb::ActorId>{a, b}, /*k=*/0, qb::duration{1s}, true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_q_size.load(), 0);     // k == 0 → resolves immediately with nothing
    EXPECT_FALSE(g_q_timeout.load());  // …and does NOT wait out the window
}

TEST(AskQuorum, KClampedToN) {
    reset_quorum();
    qb::Main   main;
    const auto a = main.addActor<FastResponder>(0);
    const auto b = main.addActor<FastResponder>(0);
    main.addActor<QuorumAsker>(0, std::vector<qb::ActorId>{a, b}, /*k=*/5, qb::duration{1s}, true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_q_size.load(), 2);     // k > n clamped to n → both replies, no hang
    EXPECT_FALSE(g_q_timeout.load());
}

// --- ask_stream overflow: a producer floods past the buffer → next() throws stream_overflow_error. ---
namespace {
std::atomic<bool> g_stream_overflow{false};
std::atomic<int>  g_stream_overflow_n{-1};
} // namespace

class OverflowConsumer : public qb::Actor {
    qb::ActorId _prod;

public:
    explicit OverflowConsumer(qb::ActorId prod)
        : _prod(prod) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        auto prod = _prod;
        spawn([prod](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Feed f;
            f.count = 20;                                          // flood
            auto s  = qb::ask_stream(c, prod, f, 1s, /*capacity=*/2); // tiny buffer
            co_await c.sleep(60ms); // do NOT drain yet — let the producer overrun the buffer
            int n = 0;
            try {
                while (auto chunk = co_await s.next())
                    ++n;
            } catch (const qb::stream_overflow_error &) {
                g_stream_overflow.store(true); // loud failure, not a silent drop
            }
            g_stream_overflow_n.store(n);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Feed &e) {
        (void) resolve_ask(e);
    }
};

TEST(AskStream, OverflowThrows) {
    g_stream_overflow.store(false);
    g_stream_overflow_n.store(-1);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/false); // emits 20, never ends
    main.addActor<OverflowConsumer>(0, prod);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_stream_overflow.load());      // overflow surfaced as an exception
    EXPECT_LE(g_stream_overflow_n.load(), 2);   // at most the buffered chunks drained before it threw
}

// --- rate_limiter::acquire() parked on an empty bucket is cancel-on-kill (the one untested wait). ---
namespace {
std::atomic<bool> g_rl_cancelled{false};
} // namespace

class RateLimitCancelActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto rl = std::make_shared<qb::rate_limiter>(1.0, 10s); // 1 token, refills very slowly
            co_await rl->acquire(c);                                // takes the only token immediately
            try {
                co_await rl->acquire(c); // parks (~10s) → must be cancelled on kill, not hang
            } catch (const qb::io::async::cancelled_error &) {
                g_rl_cancelled.store(true);
            }
        });
        co_return true;
    }
};

TEST(RateLimiter, AcquireCancelledOnKill) {
    g_rl_cancelled.store(false);
    qb::Main   main;
    const auto victim = main.addActor<RateLimitCancelActor>(0);
    main.addActor<BulkheadKiller>(0, victim); // generic kill-then-stop helper
    main.start(false);
    main.join(); // must NOT hang — the cancellation-aware acquire retracts the parked waiter
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_rl_cancelled.load());
}

// --- require<T>: returns empty on the window when no actor of that type exists (and ignores
//     live actors of OTHER types). ---
namespace {
std::atomic<int> g_disc_empty{-1};
} // namespace

class GhostWorker : public qb::Actor { // a type that is never instantiated
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

class RequireEmptyProbe : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            auto found = co_await qb::require<GhostWorker>(c, 120ms);
            g_disc_empty.store(static_cast<int>(found.size()));
            qb::Main::stop();
        });
        co_return true;
    }
};

TEST(Discovery, RequireEmptyWhenNoneOfType) {
    qb::io::async::listener::current.clear();
    g_disc_empty.store(-1);
    qb::Main main;
    main.addActor<DiscWorker>(0); // a live actor of a DIFFERENT type — must not match
    main.addActor<RequireEmptyProbe>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_disc_empty.load(), 0); // window elapsed with no GhostWorker → empty
}

// --- batcher::flush(): manual drain (the documented shutdown-drain path). ---
namespace {
std::atomic<int> g_bflush_flushes{0};
std::atomic<int> g_bflush_items{0};
} // namespace

class BatchManualFlushActor : public qb::Actor {
    qb::batcher<int> _batch{100, 5s, [](std::vector<int> &&b) {
                                g_bflush_flushes.fetch_add(1);
                                g_bflush_items.fetch_add(static_cast<int>(b.size()));
                            }};

public:
    qb::io::async::task<bool>
    onInit() override {
        _batch.add(context(), 1); // below the count threshold, long window → no auto-flush
        _batch.add(context(), 2);
        _batch.add(context(), 3);
        _batch.flush();           // manual drain — flushes the 3 buffered items now
        _batch.flush();           // no-op on an empty buffer (must not double-flush)
        qb::io::async::callback([this] { if (is_alive()) kill(); }, 10ms);
        co_return true;
    }
};

TEST(Batcher, ManualFlushDrains) {
    g_bflush_flushes.store(0);
    g_bflush_items.store(0);
    qb::Main main;
    main.addActor<BatchManualFlushActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_bflush_flushes.load(), 1); // exactly one flush (second flush() was an empty no-op)
    EXPECT_EQ(g_bflush_items.load(), 3);   // all three buffered items drained
}
