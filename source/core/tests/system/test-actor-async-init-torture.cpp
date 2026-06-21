/**
 * @file test-actor-async-init-torture.cpp
 * @brief Adversarial / exhaustive tests for async actor init crossed with the rest of qb:
 *        the pattern library inside `onInit` (ask_all / ask_retry / run_saga), multicore
 *        (cross-core stash + kill), dependency trees, stress (N concurrent, stash overflow),
 *        the activation deadline (mutual-init + stuck-init), multi-suspension and exceptions.
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
#include <thread>
#include <vector>

using namespace qb;
using namespace std::chrono_literals;

namespace {

// Shared request/response exchange + a one-shot responder used by the pattern tests.
struct Cfg : qb::Request<int> {
    int key{0};
    Cfg() = default;
    explicit Cfg(int k)
        : key(k) {}
};

// Responds to `kills_after` requests then kills itself (so the engine can drain).
class CfgService : public qb::Actor {
    int _remaining;

public:
    explicit CfgService(int kills_after = 1)
        : _remaining(kills_after) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key * 10; });
        if (--_remaining <= 0)
            kill();
    }
};

// RAII for the process-global activation deadline knob (restored after the test).
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
// B. PATTERN LIBRARY INSIDE onInit (unblocked by the ask-reply gate)
// ===========================================================================

std::atomic<int> g_askall_sum{-1};

class AsksAllInInit : public qb::Actor {
    std::vector<qb::ActorId> _svcs;

public:
    explicit AsksAllInInit(std::vector<qb::ActorId> svcs)
        : _svcs(std::move(svcs)) {}
    qb::io::async::task<bool>
    onInit() override {
        auto replies = co_await qb::ask_all(context(), _svcs, Cfg{3}, 2s);
        int  sum     = 0;
        for (auto const &r : replies)
            sum += r.response; // each = 3*10 = 30
        g_askall_sum.store(sum);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, AskAllInsideOnInit) {
    g_askall_sum.store(-1);
    qb::Main main;
    const auto a = main.addActor<CfgService>(0);
    const auto b = main.addActor<CfgService>(0);
    main.addActor<AsksAllInInit>(0, std::vector<qb::ActorId>{a, b});
    main.start(false);
    main.join();
    EXPECT_EQ(g_askall_sum.load(), 60); // 30 + 30
    EXPECT_FALSE(main.hasError());
}

std::atomic<int> g_retry_val{-1};

class AsksRetryInInit : public qb::Actor {
    qb::ActorId _svc;

public:
    explicit AsksRetryInInit(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        auto r = co_await qb::ask_retry(context(), _svc, Cfg{5}, 500ms, qb::retry_policy{.max_attempts = 3});
        g_retry_val.store(r.response);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, AskRetryInsideOnInit) {
    g_retry_val.store(-1);
    qb::Main main;
    const auto svc = main.addActor<CfgService>(0);
    main.addActor<AsksRetryInInit>(0, svc);
    main.start(false);
    main.join();
    EXPECT_EQ(g_retry_val.load(), 50);
    EXPECT_FALSE(main.hasError());
}

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

TEST(AsyncInitTorture, RunSagaInsideOnInit) {
    g_saga_steps.store(0);
    qb::Main main;
    main.addActor<RunsSagaInInit>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_saga_steps.load(), 2);
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// C. MULTICORE
// ===========================================================================

struct Tick : qb::Event {
    int n{0};
    Tick() = default;
    explicit Tick(int v)
        : n(v) {}
};

std::atomic<int>  g_xc_count{0};
std::atomic<bool> g_xc_inited{false};
std::atomic<bool> g_xc_after{true};

class XCoreSlow : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(40ms);
        g_xc_inited.store(true);
        co_return true;
    }
    void
    on(Tick &) {
        if (!g_xc_inited.load())
            g_xc_after.store(false);
        if (g_xc_count.fetch_add(1) + 1 == 4)
            kill();
    }
};

class XCoreSender : public qb::Actor {
    qb::ActorId _t;

public:
    explicit XCoreSender(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 4; ++i)
            push<Tick>(_t, i); // cross-core unicast to an Activating actor → stashed remotely
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, CrossCoreUnicastToActivatingIsStashed) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "needs >= 2 cores";
    g_xc_count.store(0);
    g_xc_inited.store(false);
    g_xc_after.store(true);
    qb::Main   main;
    const auto slow = main.addActor<XCoreSlow>(1); // Activating on core 1
    main.addActor<XCoreSender>(0, slow);           // sender on core 0
    main.start(false);
    main.join();
    EXPECT_EQ(g_xc_count.load(), 4);
    EXPECT_TRUE(g_xc_after.load()); // all delivered after activation
    EXPECT_FALSE(main.hasError());
}

std::atomic<bool> g_xck_started{false};
std::atomic<bool> g_xck_done{false};
std::atomic<bool> g_xck_destroyed{false};

class XCoreLong : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_xck_started.store(true);
        co_await context().sleep(500ms);
        g_xck_done.store(true);
        co_return true;
    }
    ~XCoreLong() override {
        g_xck_destroyed.store(true);
    }
};

class XCoreKiller : public qb::Actor {
    qb::ActorId _t;

public:
    explicit XCoreKiller(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        push<qb::KillEvent>(_t); // cross-core unicast kill of an Activating actor
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, CrossCoreKillOfActivatingActor) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "needs >= 2 cores";
    g_xck_started.store(false);
    g_xck_done.store(false);
    g_xck_destroyed.store(false);
    qb::Main   main;
    const auto t = main.addActor<XCoreLong>(1);
    main.addActor<XCoreKiller>(0, t);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_xck_started.load());
    EXPECT_FALSE(g_xck_done.load());
    EXPECT_TRUE(g_xck_destroyed.load());
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// D. DEPENDENCY TREES
// ===========================================================================

std::atomic<int> g_tree_activated{0};

class TreeLeaf : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(15ms);
        g_tree_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

class TreeRoot : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Parent itself async-inits while spawning two async-init children.
        addRefActor<TreeLeaf>();
        co_await context().sleep(10ms);
        addRefActor<TreeLeaf>();
        g_tree_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, TreeOfAsyncInitActors) {
    g_tree_activated.store(0);
    qb::Main main;
    main.addActor<TreeRoot>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_tree_activated.load(), 3); // root + 2 leaves
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// E. STRESS / DEADLINE
// ===========================================================================

std::atomic<int> g_n_activated{0};

class OneShotAsync : public qb::Actor {
    int _ms;

public:
    explicit OneShotAsync(int ms)
        : _ms(ms) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(_ms));
        g_n_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, ManyConcurrentAsyncInits) {
    g_n_activated.store(0);
    constexpr int N = 40;
    qb::Main      main;
    for (int i = 0; i < N; ++i)
        main.addActor<OneShotAsync>(0, 1 + (i % 30));
    main.start(false);
    main.join();
    EXPECT_EQ(g_n_activated.load(), N);
    EXPECT_FALSE(main.hasError());
}

std::atomic<bool> g_overflow_destroyed{false};

class StashOverflowVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(80ms); // stay Activating while the flood arrives
        co_return true;
    }
    void
    on(Tick &) {} // never reached: the stash overflows and fails the activation first
    ~StashOverflowVictim() override {
        g_overflow_destroyed.store(true);
    }
};

class Flooder : public qb::Actor {
    qb::ActorId _t;

public:
    explicit Flooder(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 6000; ++i) // > kActivationStashCap (4096) → activation fails
            push<Tick>(_t, i);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, StashOverflowFailsActivation) {
    g_overflow_destroyed.store(false);
    qb::Main   main;
    const auto victim = main.addActor<StashOverflowVictim>(0);
    main.addActor<Flooder>(0, victim);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_overflow_destroyed.load()); // overflow forced the activation to fail + remove it
    EXPECT_FALSE(main.hasError());
}

// A peer that stays Activating well past the deadline and never answers an ask.
class StuckPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5s); // far longer than the test deadline
        co_return true;
    }
};

std::atomic<bool> g_asker_destroyed{false};

class AsksStuckPeer : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksStuckPeer(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer is Activating → this request is stashed and never answered. The asker's
        // OWN activation deadline cancels the scope; the ask throws cancelled_error, which we
        // deliberately let propagate so the init FAILS (catching + co_return true would wrongly
        // succeed). This is the deadlock-proofing the deadline guarantees.
        co_await qb::ask(context(), _peer, Cfg{1}, 5s);
        co_return true; // unreachable — the deadline cancels first
    }
    ~AsksStuckPeer() override {
        g_asker_destroyed.store(true);
    }
};

TEST(AsyncInitTorture, AskToStuckActivatingPeerBrokenByDeadline) {
    g_asker_destroyed.store(false);
    ScopedDeadline dl(200'000'000); // 200 ms
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AsksStuckPeer>(0, peer);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_asker_destroyed.load()); // deadline cancelled the stuck ask → init failed → removed
}

std::atomic<bool> g_stuck_destroyed{false};
std::atomic<bool> g_stuck_completed{false};

class StuckInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().until_cancelled(); // parks forever — only the deadline frees it
        g_stuck_completed.store(true);         // must NOT happen
        co_return true;
    }
    ~StuckInit() override {
        g_stuck_destroyed.store(true);
    }
};

TEST(AsyncInitTorture, StuckInitKilledByDeadline) {
    g_stuck_destroyed.store(false);
    g_stuck_completed.store(false);
    ScopedDeadline dl(200'000'000);
    qb::Main       main;
    main.addActor<StuckInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_stuck_destroyed.load());
    EXPECT_FALSE(g_stuck_completed.load());
}

// ===========================================================================
// F. CORNER CASES
// ===========================================================================

std::atomic<int> g_chain_steps{0};

class MultiSuspendInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        co_await context().sleep(5ms);
        g_chain_steps.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, MultiSuspensionChain) {
    g_chain_steps.store(0);
    qb::Main main;
    main.addActor<MultiSuspendInit>(0);
    main.start(false);
    main.join();
    EXPECT_EQ(g_chain_steps.load(), 3);
    EXPECT_FALSE(main.hasError());
}

std::atomic<bool> g_throw_destroyed{false};

class ThrowingInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10ms);
        throw std::runtime_error("init blew up after a suspension");
        co_return true; // unreachable
    }
    ~ThrowingInit() override {
        g_throw_destroyed.store(true);
    }
};

TEST(AsyncInitTorture, ExceptionAfterSuspensionFailsInit) {
    g_throw_destroyed.store(false);
    qb::Main main;
    main.addActor<ThrowingInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_throw_destroyed.load()); // uncaught throw → init failed → actor removed
}

// ===========================================================================
// G. ActorHandle phase-awareness (Milestone 2): ready() / ready_async() / id()-stash
// ===========================================================================

std::atomic<bool> g_h_served{false};
std::atomic<bool> g_h_ready_seen{false};
std::atomic<bool> g_h_unready_seen{false};

class HandleChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(30ms); // Activating window
        co_return true;
    }
    void
    on(Tick &) {
        g_h_served.store(true); // the event pushed to id() while Activating, replayed after
    }
};

class HandleParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<HandleChild>();
        // The async child is Activating → handle is valid (id known) but NOT ready.
        g_h_unready_seen.store(child.valid() && !child.ready());
        push<Tick>(child.id(), 1); // safe: stashed until the child activates
        // Block until the child is active (poll-based, cancellation-aware).
        const bool ok = co_await child.ready_async(context(), 2s);
        g_h_ready_seen.store(ok && child.ready());
        if (child.ready())
            child->kill(); // deref-when-ready is now safe
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, ActorHandleReadyAsyncAndIdStash) {
    g_h_served.store(false);
    g_h_ready_seen.store(false);
    g_h_unready_seen.store(false);
    qb::Main main;
    main.addActor<HandleParent>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_h_unready_seen.load()); // handle was valid-but-not-ready while Activating
    EXPECT_TRUE(g_h_ready_seen.load());   // ready_async resolved once the child activated
    EXPECT_TRUE(g_h_served.load());       // the id()-addressed event was stashed + replayed
    EXPECT_FALSE(main.hasError());
}

// A sync-init child is ready the instant addRefActor returns (no co_await needed).
std::atomic<bool> g_sync_ready_now{false};

class SyncChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true; // synchronous → active immediately
    }
};

class SyncParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<SyncChild>();
        g_sync_ready_now.store(child.ready()); // true at once — no Activating phase
        if (child.ready())
            child->kill();
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, SyncChildHandleReadyImmediately) {
    g_sync_ready_now.store(false);
    qb::Main main;
    main.addActor<SyncParent>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_sync_ready_now.load());
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// H. patterns inside onInit whose TARGET is ALSO Activating — the load-bearing
//    case the ask-reply gate exists to solve (request stashed remotely, reply
//    delivered once both activate). Existing tests only used sync responders.
// ===========================================================================

// A responder that is itself Activating while it receives the asker's request.
class AsyncResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_await context().sleep(25ms); // Activating — the asker's request stashes here
        co_return true;
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key * 10; });
        kill();
    }
};

std::atomic<int> g_both_val{-1};

class AsyncAsker : public qb::Actor {
    qb::ActorId _r;

public:
    explicit AsyncAsker(qb::ActorId r)
        : _r(r) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5ms); // be Activating on the asker side too
        auto reply = co_await qb::ask(context(), _r, Cfg{4}, 2s);
        g_both_val.store(reply.response);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, OnInitAskActivatingPeerAndWaitResolves) {
    g_both_val.store(-1);
    qb::Main   main;
    const auto r = main.addActor<AsyncResponder>(0);
    main.addActor<AsyncAsker>(0, r);
    main.start(false);
    main.join();
    EXPECT_EQ(g_both_val.load(), 40); // request stashed at the Activating responder, reply delivered
    EXPECT_FALSE(main.hasError());
}

std::atomic<int> g_any_val{-1};

class AskAnyInInit : public qb::Actor {
    std::vector<qb::ActorId> _targets;

public:
    explicit AskAnyInInit(std::vector<qb::ActorId> t)
        : _targets(std::move(t)) {}
    qb::io::async::task<bool>
    onInit() override {
        auto reply = co_await qb::ask_any(context(), _targets, Cfg{6}, 2s);
        g_any_val.store(reply.response);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, OnInitAskAnyToActivatingTargetResolves) {
    g_any_val.store(-1);
    qb::Main   main;
    const auto slow = main.addActor<AsyncResponder>(0); // Activating target
    const auto fast = main.addActor<CfgService>(0);     // already-active target — wins
    main.addActor<AskAnyInInit>(0, std::vector<qb::ActorId>{slow, fast});
    main.start(false);
    main.join();
    EXPECT_EQ(g_any_val.load(), 60); // ask_any resolves on the first (fast) reply
    EXPECT_FALSE(main.hasError());
}

std::atomic<int> g_guarded_val{-1};

class AskGuardedInInit : public qb::Actor {
    qb::ActorId                         _svc;
    std::shared_ptr<qb::CircuitBreaker> _breaker = std::make_shared<qb::CircuitBreaker>(3u, 100ms);

public:
    explicit AskGuardedInInit(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        auto reply = co_await qb::ask_guarded(context(), _breaker, _svc, Cfg{8}, 1s);
        g_guarded_val.store(reply.response);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, OnInitAskGuardedAcrossActivationBoundary) {
    g_guarded_val.store(-1);
    qb::Main   main;
    const auto svc = main.addActor<CfgService>(0);
    main.addActor<AskGuardedInInit>(0, svc);
    main.start(false);
    main.join();
    EXPECT_EQ(g_guarded_val.load(), 80); // breaker closed → guarded ask succeeds during Activating
    EXPECT_FALSE(main.hasError());
}

std::atomic<bool> g_retry_dl_failed{false};

class AskRetryDeadlineInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskRetryDeadlineInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer (StuckPeer) never answers; each attempt times out and retries — but the
        // per-actor activation deadline cancels the WHOLE loop, not just one attempt.
        co_await qb::ask_retry(context(), _peer, Cfg{1}, 80ms, qb::retry_policy{.max_attempts = 5});
        co_return true; // unreachable — the deadline cancels first
    }
    ~AskRetryDeadlineInInit() override {
        g_retry_dl_failed.store(true);
    }
};

TEST(AsyncInitTorture, OnInitAskRetryBrokenByActivationDeadline) {
    g_retry_dl_failed.store(false);
    ScopedDeadline dl(250'000'000); // 250 ms < 5 x 80 ms of retries
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AskRetryDeadlineInInit>(0, peer);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_retry_dl_failed.load()); // deadline cancelled the retry loop → init failed
}

// ===========================================================================
// I. shutdown + ActorHandle::ready_async edge cases
// ===========================================================================

std::atomic<bool> g_shut_started{false};
std::atomic<bool> g_shut_completed{false};
std::atomic<bool> g_shut_destroyed{false};

class ShutdownVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_shut_started.store(true);
        co_await context().sleep(500ms); // cancelled by the broadcast shutdown
        g_shut_completed.store(true);     // must NOT happen
        co_return true;
    }
    ~ShutdownVictim() override {
        g_shut_destroyed.store(true);
    }
};

class ShutdownTrigger : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        broadcast<qb::KillEvent>(); // a broadcast shutdown reaches the still-Activating victim
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, BroadcastShutdownDuringAsyncOnInitCancelsCleanly) {
    g_shut_started.store(false);
    g_shut_completed.store(false);
    g_shut_destroyed.store(false);
    qb::Main main;
    main.addActor<ShutdownVictim>(0);
    main.addActor<ShutdownTrigger>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_shut_started.load());
    EXPECT_FALSE(g_shut_completed.load()); // init cancelled mid-flight
    EXPECT_TRUE(g_shut_destroyed.load());
    EXPECT_FALSE(main.hasError());
}

// A child that never activates within any test window (killed via broadcast at teardown).
class NeverReadyChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10s);
        co_return true;
    }
};

std::atomic<bool> g_ra_timedout{false};

class WaitsThenTimesOut : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child   = addRefActor<NeverReadyChild>();
        const bool ok = co_await child.ready_async(context(), 80ms); // child never activates
        g_ra_timedout.store(!ok);
        broadcast<qb::KillEvent>(); // tear down the stuck child + self
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, ReadyAsyncTimesOutWhenChildNeverActivates) {
    g_ra_timedout.store(false);
    qb::Main main;
    main.addActor<WaitsThenTimesOut>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_ra_timedout.load()); // ready_async returned false on timeout
    EXPECT_FALSE(main.hasError());
}

std::atomic<bool> g_pk_destroyed{false};

class WaiterParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        addRefActor<NeverReadyChild>();
        auto child2 = addRefActor<NeverReadyChild>();
        co_await child2.ready_async(context(), 10s); // long wait — we are killed first
        co_return true;                              // unreachable
    }
    ~WaiterParent() override {
        g_pk_destroyed.store(true);
    }
};

class WaiterKiller : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(30ms);
        broadcast<qb::KillEvent>(); // kill the parent mid-ready_async + its stuck children
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, ParentKilledWhileAwaitingReadyAsyncUnwinds) {
    g_pk_destroyed.store(false);
    qb::Main main;
    main.addActor<WaiterParent>(0);
    main.addActor<WaiterKiller>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_pk_destroyed.load()); // ready_async wait unwound cleanly on kill
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// J. gate edges: multi-sender stash, custom (non-Kill) broadcast passes
// ===========================================================================

std::atomic<int> g_ms_count{0};
std::atomic<bool> g_ms_after{true};

class MultiStashVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Tick>(*this);
        co_await context().sleep(40ms);
        g_ms_inited.store(true);
        co_return true;
    }
    void
    on(Tick &) {
        if (!g_ms_inited.load())
            g_ms_after.store(false);
        if (g_ms_count.fetch_add(1) + 1 == 9) // 3 senders x 3 events
            kill();
    }

    static inline std::atomic<bool> g_ms_inited{false};
};

class MiniSender : public qb::Actor {
    qb::ActorId _t;

public:
    explicit MiniSender(qb::ActorId t)
        : _t(t) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 3; ++i)
            push<Tick>(_t, i);
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, MultipleSendersAllStashedAndReplayed) {
    g_ms_count.store(0);
    g_ms_after.store(true);
    MultiStashVictim::g_ms_inited.store(false);
    qb::Main   main;
    const auto v = main.addActor<MultiStashVictim>(0);
    main.addActor<MiniSender>(0, v);
    main.addActor<MiniSender>(0, v);
    main.addActor<MiniSender>(0, v);
    main.start(false);
    main.join();
    EXPECT_EQ(g_ms_count.load(), 9); // all 9 stashed across 3 senders, replayed after activation
    EXPECT_TRUE(g_ms_after.load());
    EXPECT_FALSE(main.hasError());
}

struct Ping2 : qb::Event {};
std::atomic<bool> g_cb_received{false};

class BcastVictim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping2>(*this);
        co_await context().sleep(40ms); // still Activating when the broadcast arrives
        co_return true;
    }
    void
    on(Ping2 &) {
        g_cb_received.store(true); // a broadcast is NOT stashed — it passes the gate at once
        kill();
    }
};

class Bcaster : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(10ms);
        broadcast<Ping2>();
        kill();
        co_return true;
    }
};

TEST(AsyncInitTorture, CustomBroadcastPassesGateWhileActivating) {
    g_cb_received.store(false);
    qb::Main main;
    main.addActor<BcastVictim>(0);
    main.addActor<Bcaster>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_cb_received.load()); // custom broadcast reached the Activating actor (not stashed)
    EXPECT_FALSE(main.hasError());
}

} // namespace
