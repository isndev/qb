/**
 * @file test-actor-async-init.cpp
 * @brief Integration tests for asynchronous actor initialization (`task<bool> onInit()`).
 *
 * Exercises the *Activating* phase end-to-end: an `onInit()` that performs a `co_await`
 * suspends, the owning core keeps running, and:
 *   - inbound unicast business events are stashed and replayed FIFO once active,
 *   - a unicast `KillEvent` still reaches an Activating actor (kill-during-init),
 *   - a kill mid-init cancels the coroutine and the actor outlives its own frame
 *     (deferred destroy) before being torn down,
 *   - a `co_return false` after a suspension removes the actor.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites
 * (the coroutine frame pool is deliberately not drained at thread exit).
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

using namespace qb;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// 1. An onInit that co_awaits completes, then the actor runs normally.
// ---------------------------------------------------------------------------
std::atomic<bool> g_t1_completed{false};

class AwaitThenRunActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // suspends → Activating; core keeps serving
        g_t1_completed.store(true);
        kill(); // nothing else to do — let the engine drain
        co_return true;
    }
};

TEST(ActorAsyncInit, AwaitingOnInitCompletesAndActivates) {
    qb::Main main;
    main.addActor<AwaitThenRunActor>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_t1_completed.load());
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 2. Unicast business events sent while Activating are stashed and replayed
//    FIFO *after* activation completes.
// ---------------------------------------------------------------------------
struct OrderProbe : public qb::Event {
    int seq{0};
    OrderProbe() = default;
    explicit OrderProbe(int s)
        : seq(s) {}
};

std::atomic<bool> g_slow_inited{false};
std::atomic<int>  g_slow_count{0};
std::atomic<bool> g_order_ok{true};
std::atomic<bool> g_all_after_init{true};

class SlowConsumer : public qb::Actor {
    int _expected_next = 1;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OrderProbe>(*this);
        co_await context().sleep(40ms); // long enough for the burst to pile up while Activating
        g_slow_inited.store(true);
        co_return true;
    }

    void
    on(OrderProbe &e) {
        if (!g_slow_inited.load())
            g_all_after_init.store(false); // a stashed event must never land before activation
        if (e.seq != _expected_next)
            g_order_ok.store(false); // FIFO order must be preserved
        ++_expected_next;
        if (g_slow_count.fetch_add(1) + 1 == 5)
            kill();
    }
};

class BurstSender : public qb::Actor {
    qb::ActorId _target;

public:
    explicit BurstSender(qb::ActorId target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        // Synchronous init: fire the whole burst while SlowConsumer is still Activating.
        for (int i = 1; i <= 5; ++i)
            push<OrderProbe>(_target, i);
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, StashedEventsReplayedInOrderAfterActivation) {
    g_slow_inited.store(false);
    g_slow_count.store(0);
    g_order_ok.store(true);
    g_all_after_init.store(true);

    qb::Main   main;
    const auto slow = main.addActor<SlowConsumer>(0);
    main.addActor<BurstSender>(0, slow);
    main.start(false);
    main.join();

    EXPECT_EQ(g_slow_count.load(), 5);    // all five replayed
    EXPECT_TRUE(g_order_ok.load());       // in FIFO order
    EXPECT_TRUE(g_all_after_init.load()); // never before onInit completed
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 3. Killing an actor mid-async-init cancels the coroutine and the actor is
//    destroyed cleanly (deferred destroy: it outlives its own onInit frame).
// ---------------------------------------------------------------------------
std::atomic<bool> g_t3_started{false};
std::atomic<bool> g_t3_completed{false};
std::atomic<bool> g_t3_destroyed{false};

class LongInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_t3_started.store(true);
        co_await context().sleep(500ms); // cancelled by the kill long before this elapses
        g_t3_completed.store(true);      // must NOT be reached
        co_return true;
    }
    ~LongInitActor() override {
        g_t3_destroyed.store(true);
    }
};

class KillerActor : public qb::Actor {
    qb::ActorId _target;

public:
    explicit KillerActor(qb::ActorId target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // let LongInitActor enter its long init first
        push<qb::KillEvent>(_target);   // unicast kill must reach an Activating actor
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, KillDuringInitCancelsAndDestroysCleanly) {
    g_t3_started.store(false);
    g_t3_completed.store(false);
    g_t3_destroyed.store(false);

    qb::Main   main;
    const auto target = main.addActor<LongInitActor>(0);
    main.addActor<KillerActor>(0, target);
    main.start(false);
    main.join();

    EXPECT_TRUE(g_t3_started.load());
    EXPECT_FALSE(g_t3_completed.load()); // init was cancelled, not completed
    EXPECT_TRUE(g_t3_destroyed.load());  // actor torn down after the frame unwound
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 4. An async onInit that co_returns false after suspending removes the actor.
// ---------------------------------------------------------------------------
std::atomic<bool> g_t4_destroyed{false};

class AsyncFailActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(15ms);
        co_return false; // initialization failed after suspending → actor removed
    }
    ~AsyncFailActor() override {
        g_t4_destroyed.store(true);
    }
};

TEST(ActorAsyncInit, AsyncInitFailureRemovesActor) {
    g_t4_destroyed.store(false);
    qb::Main main;
    main.addActor<AsyncFailActor>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_t4_destroyed.load());
}

// ---------------------------------------------------------------------------
// 5. Several actors with independent async inits all activate concurrently.
// ---------------------------------------------------------------------------
std::atomic<int> g_t5_activated{0};

class ConcurrentInitActor : public qb::Actor {
    int _ms;

public:
    explicit ConcurrentInitActor(int ms)
        : _ms(ms) {}

    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(_ms));
        g_t5_activated.fetch_add(1);
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, MultipleActorsActivateConcurrently) {
    g_t5_activated.store(0);
    qb::Main main;
    main.addActor<ConcurrentInitActor>(0, 10);
    main.addActor<ConcurrentInitActor>(0, 25);
    main.addActor<ConcurrentInitActor>(0, 40);
    main.start(false);
    main.join();
    EXPECT_EQ(g_t5_activated.load(), 3);
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 6/7. onInit that `co_await qb::ask(...)` a peer to fetch its config — the
//      flagship async-init use case. Exercises the ask-reply gate: the in-flight
//      ask's reply must reach the Activating actor instead of being stashed.
// ---------------------------------------------------------------------------
struct ConfigReq : qb::Request<int> {
    int key{0};
    ConfigReq() = default;
    explicit ConfigReq(int k)
        : key(k) {}
};

class ConfigService : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ConfigReq>(*this);
        co_return true; // sync — active before anyone asks
    }
    void
    on(ConfigReq &e) {
        qb::answer(*this, e, [](ConfigReq const &r) { return r.key * 10; });
        kill(); // one-shot responder: let the engine drain once the asker is served
    }
};

std::atomic<bool> g_ask_init_ok{false};
std::atomic<int>  g_ask_value{-1};

class AsksConfigInInit : public qb::Actor {
    qb::ActorId _svc;

public:
    explicit AsksConfigInInit(qb::ActorId svc)
        : _svc(svc) {}

    qb::io::async::task<bool>
    onInit() override {
        // The reply lands while we are still Activating → the gate must deliver it here,
        // not stash it (otherwise this init deadlocks on its own reply).
        auto reply = co_await qb::ask(context(), _svc, ConfigReq{7}, std::chrono::seconds{2});
        g_ask_value.store(reply.response);
        g_ask_init_ok.store(true);
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, OnInitCanAskAPeerSameCore) {
    g_ask_init_ok.store(false);
    g_ask_value.store(-1);
    qb::Main   main;
    const auto svc = main.addActor<ConfigService>(0);
    main.addActor<AsksConfigInInit>(0, svc);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_ask_init_ok.load());
    EXPECT_EQ(g_ask_value.load(), 70);
    EXPECT_FALSE(main.hasError());
}

TEST(ActorAsyncInit, OnInitCanAskAPeerCrossCore) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "needs >= 2 cores";
    g_ask_init_ok.store(false);
    g_ask_value.store(-1);
    qb::Main   main;
    const auto svc = main.addActor<ConfigService>(1); // responder on core 1
    main.addActor<AsksConfigInInit>(0, svc);          // asker (Activating) on core 0
    main.start(false);
    main.join();
    EXPECT_TRUE(g_ask_init_ok.load());
    EXPECT_EQ(g_ask_value.load(), 70);
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 8. Drive outcomes on the SYNCHRONOUS path (no co_await): co_return false and a
//    thrown exception must both fail the init exactly like their suspended kin.
// ---------------------------------------------------------------------------
std::atomic<bool> g_syncfalse_destroyed{false};

class SyncFalseInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false; // completes synchronously → __drive_init__ ReadyFalse
    }
    ~SyncFalseInit() override {
        g_syncfalse_destroyed.store(true);
    }
};

TEST(ActorAsyncInit, SyncOnInitReturnsFalseWithoutCoAwait) {
    g_syncfalse_destroyed.store(false);
    qb::Main main;
    main.addActor<SyncFalseInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError());              // initial-actor sync init failure aborts start
    EXPECT_TRUE(g_syncfalse_destroyed.load()); // actor removed
}

std::atomic<bool> g_syncthrow_destroyed{false};

class SyncThrowInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        if (id().is_valid()) // always true; defeats unreachable-code analysis on the co_return
            throw std::runtime_error("init blew up synchronously");
        co_return true;
    }
    ~SyncThrowInit() override {
        g_syncthrow_destroyed.store(true);
    }
};

TEST(ActorAsyncInit, SyncOnInitThrowsWithoutCoAwait) {
    g_syncthrow_destroyed.store(false);
    qb::Main main;
    main.addActor<SyncThrowInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(main.hasError()); // uncaught sync throw ⇒ BadActorInit
    EXPECT_TRUE(g_syncthrow_destroyed.load());
}

// ---------------------------------------------------------------------------
// 9. A ServiceActor singleton with an ASYNC onInit goes through Activating, and
//    an event addressed to it meanwhile is stashed + replayed once it activates.
// ---------------------------------------------------------------------------
struct AsyncSvcTag {};
struct Poke : qb::Event {};

std::atomic<bool> g_svc_inited{false};
std::atomic<bool> g_svc_served_after{true};

class AsyncService : public qb::ServiceActor<AsyncSvcTag> {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Poke>(*this);
        co_await context().sleep(30ms); // service is Activating during this window
        g_svc_inited.store(true);
        co_return true;
    }
    void
    on(Poke &) {
        if (!g_svc_inited.load())
            g_svc_served_after.store(false); // a stashed event must land only after activation
        kill();
    }
};

class ServicePoker : public qb::Actor {
    qb::ActorId _svc;

public:
    explicit ServicePoker(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        push<Poke>(_svc); // sent while the service is still Activating → stashed
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, ServiceActorWithAsyncOnInitStashesAndServes) {
    g_svc_inited.store(false);
    g_svc_served_after.store(true);
    qb::Main   main;
    const auto svc = main.addActor<AsyncService>(0);
    main.addActor<ServicePoker>(0, svc);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_svc_inited.load());
    EXPECT_TRUE(g_svc_served_after.load()); // stash replayed only after the service activated
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 10. A disabled activation deadline (activation_deadline_ns == 0) never force-fails
//     an Activating actor — a slow init still completes naturally.
// ---------------------------------------------------------------------------
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

std::atomic<bool> g_dd_activated{false};

class SlowButFineInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(60ms);
        g_dd_activated.store(true);
        kill();
        co_return true;
    }
};

TEST(ActorAsyncInit, DisabledDeadlineNeverTimesOutActivating) {
    g_dd_activated.store(false);
    ScopedDeadline dl(0); // disable the deadline
    qb::Main       main;
    main.addActor<SlowButFineInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_dd_activated.load()); // completed naturally, not force-failed
    EXPECT_FALSE(main.hasError());
}

} // namespace
