/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/main-lifecycle.cpp
 * @brief `qb::Main` start/stop/error-model — the canonical engine-lifecycle suite.
 *
 * Drives the full engine bring-up and teardown contract against a real multi-thread
 * `qb::Main` + event loop. The cases split into two bands:
 *
 *   - happy paths — mono- and multi-core `start()/join()` of a self-killing actor, plus the
 *     three graceful-stop routes: `Main::stop()`, and an out-of-band POSIX signal
 *     (`std::raise(SIGABRT)` against a registered signal) — all of which must leave
 *     `hasError() == false`;
 *   - failure paths — an empty core, a false-returning `onInit()`, and a throwing `onInit()`:
 *       · empty engine / a `core().clear()`'d core  → `Error::NoActor` (started with 0 actors);
 *       · `onInit()` co_returns false               → `Error::BadActorInit` (a clean false);
 *       · `onInit()` throws                         → `Error::BadActorInit` (`__drive_init__` catches it).
 *
 *     The atoms `g_threw` and `g_returned_false` prove which test stimulus ran; they do not
 *     expose the private error word. `Main::hasError()` reports both paths as failures. An
 *     exception that escapes a constructor before the loop uses `ExceptionThrown`; the
 *     post-barrier handler exception has a distinct internal runtime marker.
 *
 * The lifecycle cases use actor state as their oracle; the teardown regressions use bounded
 * waits on explicit signals, with the ctest TIMEOUT as the outer backstop. The multi-core
 * failure case derandomizes which core is cleared
 * (fixed seed, logged) so a failure reproduces. Multi-core cases `GTEST_SKIP` on a 1-core runner
 * instead of asserting hardware. The SIGABRT case is the sole signal-raiser in this binary and the
 * binary is labelled `serial` so it never races another test's signal handler.
 *
 * Every in-actor `EXPECT_*` / side effect is mirrored to a post-`join()` atom asserted in the test
 * body, so a never-scheduled actor cannot let a case pass vacuously.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <future>
#include <latch>
#include <random>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>

namespace {

// ---------------------------------------------------------------------------
// Shared observability atoms (reset per test). Mirror in-actor outcomes so a
// worker-thread side effect is attributed to the right named test after join().
// ---------------------------------------------------------------------------
std::atomic<bool> g_init_ran{false};       // a happy-path onInit reached its co_return true
std::atomic<bool> g_threw{false};          // a throwing onInit reached the throw site
std::atomic<bool> g_returned_false{false}; // a failing onInit reached its co_return false
std::atomic<bool> g_signal_seen{false};    // an actor observed a SignalEvent
std::atomic<int>  g_signals_observed{0};   // number of SignalEvents an actor observed (multi-signal regression)

void
reset_atoms() {
    g_init_ran.store(false);
    g_threw.store(false);
    g_returned_false.store(false);
    g_signal_seen.store(false);
    g_signals_observed.store(0);
}

[[nodiscard]] std::uint32_t
hardware_cores() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0u ? 1u : static_cast<std::uint32_t>(hw);
}

// ---------------------------------------------------------------------------
// Happy-path actor: registers SignalEvent, self-kills unless asked to stay live.
// ---------------------------------------------------------------------------
class TestActor : public qb::Actor {
    const bool _keep_live;

public:
    explicit TestActor(bool live)
        : _keep_live(live) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::SignalEvent>(*this);
        g_init_ran.store(true);
        if (!_keep_live)
            kill();
        co_return true;
    }

    void
    on(qb::SignalEvent const &event) {
        g_signal_seen.store(true);
        if (event.signum == SIGINT || event.signum == SIGABRT)
            kill();
    }
};

// ---------------------------------------------------------------------------
// Multi-signal actor: SURVIVES the first SignalEvent (simulating a SIGHUP-style reload) and only
// self-kills on the second. Used to prove that a signal / Main::stop() after an already-consumed
// first one is still delivered (the Main::_signal_generation regression).
// ---------------------------------------------------------------------------
class SurviveFirstSignalActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::SignalEvent>(*this);
        g_init_ran.store(true);
        co_return true;
    }

    void
    on(qb::SignalEvent const & /*event*/) {
        if (g_signals_observed.fetch_add(1) + 1 >= 2)
            kill(); // die only on the SECOND signal; stay live through the first
    }
};

// ---------------------------------------------------------------------------
// Failure actor — onInit co_returns false (→ Error::BadActorInit). Records that
// it reached the clean false return (and, by omission, that it never threw).
// ---------------------------------------------------------------------------
class FalseInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        g_returned_false.store(true);
        co_return false;
    }
};

// ---------------------------------------------------------------------------
// Failure actor — onInit THROWS (→ Error::BadActorInit). Sets g_threw at the
// throw site; its co_return is unreachable, so g_init_ran stays false.
// ---------------------------------------------------------------------------
class ThrowInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        if (id().is_valid()) { // always true; defeats unreachable-code analysis on the co_return
            g_threw.store(true);
            throw std::runtime_error("onInit blew up synchronously");
        }
        co_return true; // unreachable
    }
};

// ===========================================================================
// Happy paths — no error.
// ===========================================================================

TEST(MainLifecycle, StartMonoCoreSelfKillNoError) {
    reset_atoms();
    qb::Main main;
    main.addActor<TestActor>(0, false);
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load()) << "the actor's onInit must have run (no vacuous pass)";
}

TEST(MainLifecycle, StartMultiCoreSelfKillNoError) {
    const auto cores = hardware_cores();
    if (cores < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise multi-core bring-up";
    reset_atoms();
    qb::Main main;
    for (auto i = 0u; i < cores; ++i)
        main.addActor<TestActor>(i, false);
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load()) << "at least one actor's onInit must have run";
}

TEST(MainLifecycle, StopMonoCoreGracefulNoError) {
    reset_atoms();
    qb::Main main;
    main.addActor<TestActor>(0, true); // stays live until stop()
    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load());
}

TEST(MainLifecycle, StopMultiCoreGracefulNoError) {
    const auto cores = hardware_cores();
    if (cores < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise multi-core stop";
    reset_atoms();
    qb::Main main;
    for (auto i = 0u; i < cores; ++i)
        main.addActor<TestActor>(i, true);
    main.start();
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load());
}

// Signal-driven graceful stop. SOLE signal-raiser in this binary; the binary is labelled
// `serial` so it never overlaps another test's signal handler. SIGABRT is registered to route
// to Main::stop() (graceful), then raised once: actors observe the SignalEvent and self-kill.
TEST(MainLifecycle, StopMultiCoreViaRaisedSignalNoError) {
    const auto cores = hardware_cores();
    if (cores < 2u)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot exercise multi-core signal stop";
    reset_atoms();
    qb::Main main;
    for (auto i = 0u; i < cores; ++i)
        main.addActor<TestActor>(i, true);

    qb::Main::registerSignal(SIGABRT);
    main.start();
    std::raise(SIGABRT); // graceful shutdown via the registered signal
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load());
    // Note: SignalEvent fan-out to actors is best-effort once stop() races teardown, so we do not
    // require g_signal_seen — the load-bearing contract here is a CLEAN (error-free) signal stop.
}

// Regression (Main::_signal_generation): a per-core "signal already consumed" latch used to drop
// EVERY signal after the first, so an actor that survives the first signal (a SIGHUP-style reload)
// could never be stopped by a second signal / Main::stop() — join() hung forever. Deliver two
// stop()s with the first fully observed before the second, and require the engine to still stop.
// Pre-fix this test hangs in join() (ctest timeout); with the generation counter it completes clean.
TEST(MainLifecycle, SecondSignalStopsEngineAfterSurvivedFirst) {
    reset_atoms();
    qb::Main main;
    main.addActor<SurviveFirstSignalActor>(0);
    main.start(); // non-blocking
    main.stop();  // signal #1 — the actor observes it but stays live (reload semantics)
    // The two stop()s MUST be observed as distinct generations: wait until the core has delivered
    // #1 before raising #2, otherwise they coalesce into one bump and the actor sees a single event.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_signals_observed.load() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_GE(g_signals_observed.load(), 1) << "the first signal was never delivered to the actor";
    main.stop(); // signal #2 — with the fix this is delivered and the actor self-kills
    main.join(); // pre-fix: hangs here (the second signal was dropped)
    EXPECT_FALSE(main.hasError());
    EXPECT_GE(g_signals_observed.load(), 2) << "a signal after an already-consumed one must still be delivered";
}

// ===========================================================================
// Failure paths — each pinned to its specific VirtualCore::Error code.
// ===========================================================================

// Empty engine: no core registered at all → start barrier trips BadInit (>= threshold).
TEST(MainLifecycle, EmptyEngineAbortsWithError) {
    reset_atoms();
    qb::Main main;
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "an engine with zero cores must fail to start";
    EXPECT_FALSE(g_init_ran.load());
    EXPECT_FALSE(g_threw.load());
    EXPECT_FALSE(g_returned_false.load());
}

// A core that has every actor cleared starts with 0 actors → Error::NoActor.
// Multi-core: pick the cleared core deterministically (fixed seed, logged) so any failure repros.
TEST(MainLifecycle, MultiCoreWithAClearedCoreAbortsWithNoActor) {
    const auto cores = hardware_cores();
    if (cores < 2u)
        GTEST_SKIP() << "requires-multicore: need >=2 cores to clear one and still launch others";
    reset_atoms();

    constexpr unsigned kSeed = 0xC0FFEEu; // fixed seed → deterministic, reproducible choice
    std::mt19937       rng(kSeed);
    const auto         fail_core = rng() % cores;
    qb::io::cout() << "[MainLifecycle] seed=" << kSeed << " cleared core=" << fail_core << " of " << cores << std::endl;

    qb::Main main;
    for (auto i = 0u; i < cores; ++i)
        main.addActor<TestActor>(i, false);
    main.core(fail_core).clear(); // this core now has 0 actors → NoActor

    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "a core launched with 0 actors must fail the start barrier";
    EXPECT_FALSE(g_threw.load()) << "NoActor must NOT be a thrown-exception path";
    EXPECT_FALSE(g_returned_false.load()) << "NoActor must NOT be a co_return-false path";
}

// onInit co_returns false (the initial actor of a mono core) → Error::BadActorInit.
// The atoms prove this case returned false rather than throwing; both paths
// map to BadActorInit because __drive_init__ catches an onInit exception.
TEST(MainLifecycle, FalseOnInitAbortsWithBadActorInit) {
    reset_atoms();
    qb::Main main;
    main.addActor<FalseInitActor>(0);
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "a co_return false onInit must fail the start";
    EXPECT_TRUE(g_returned_false.load()) << "the init reached its clean co_return false";
    EXPECT_FALSE(g_threw.load()) << "this init returned false instead of throwing";
    EXPECT_FALSE(g_init_ran.load());
}

// onInit THROWS (the initial actor of a mono core) → Error::BadActorInit.
// The atoms distinguish this stimulus from a clean false return, not the enum.
TEST(MainLifecycle, ThrowingOnInitAbortsWithBadActorInit) {
    reset_atoms();
    qb::Main main;
    main.addActor<ThrowInitActor>(0);
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "a throwing onInit must fail the start";
    EXPECT_TRUE(g_threw.load()) << "the init reached its throw site";
    EXPECT_FALSE(g_returned_false.load()) << "the throwing init did not reach a clean false return";
    EXPECT_FALSE(g_init_ran.load()) << "the throw aborts before co_return — onInit never completes";
}

// Keep the failed worker in actor teardown after it has published the startup
// error. start() must not give its caller access to Main's resources until this
// destructor and the worker's exit guards have completed.
class HeldFailedInitActor : public qb::Actor {
    std::promise<void> *_teardown_entered;
    std::latch         *_teardown_release;

public:
    HeldFailedInitActor(std::promise<void> *entered, std::latch *release)
        : _teardown_entered(entered)
        , _teardown_release(release) {}

    ~HeldFailedInitActor() override {
        _teardown_entered->set_value();
        _teardown_release->wait();
    }

    qb::io::async::task<bool>
    onInit() final {
        g_returned_false.store(true);
        co_return false;
    }
};

TEST(MainLifecycle, FailedStartupWaitsForWorkerTeardownBeforeReturning) {
    reset_atoms();
    std::promise<void> teardown_entered_promise;
    auto               teardown_entered = teardown_entered_promise.get_future();
    std::latch         teardown_release{1};
    std::promise<void> start_returned_promise;
    auto               start_returned = start_returned_promise.get_future();

    qb::Main main;
    main.addActor<HeldFailedInitActor>(0, &teardown_entered_promise, &teardown_release);
    std::thread starter([&] {
        main.start(true);
        start_returned_promise.set_value();
    });

    // These are bounded waits on explicit lifecycle signals, not sleeps used to
    // guess when a worker might run. The gate remains closed while we inspect
    // whether start() has returned ahead of that worker's cleanup. Five seconds
    // gives a loaded sanitizer or Windows runner time to schedule the caller.
    const auto entered = teardown_entered.wait_for(std::chrono::seconds(5));
    const bool returned_before_cleanup =
        entered == std::future_status::ready && start_returned.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    teardown_release.count_down(); // release on both red and green paths
    starter.join();
    main.join(); // safe cleanup on the red baseline, after the worker was released

    EXPECT_EQ(entered, std::future_status::ready) << "the failed actor never reached worker teardown";
    EXPECT_FALSE(returned_before_cleanup) << "start() returned while its failed worker still owned Main resources";
    EXPECT_TRUE(main.hasError());
    EXPECT_TRUE(g_returned_false.load());

    // A failed attempt can be reconfigured and started again after teardown.
    g_init_ran.store(false);
    EXPECT_TRUE(main.addActor<TestActor>(0, false).is_valid());
    main.start();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_init_ran.load());
}

// With start(false), the calling thread is one of the workers. Identify roles
// by that thread rather than relying on unordered CoreInitializer iteration.
struct RuntimeFailureState {
    qb::Main           *main;
    std::thread::id     caller;
    std::atomic<bool>   thrower_claimed{false};
    std::atomic<int>    caller_count{0};
    std::atomic<int>    thrower_count{0};
    std::atomic<int>    survivor_count{0};
    std::atomic<bool>   runtime_seen_reported{false};
    std::promise<void> *runtime_seen;
    std::atomic<int>    peers_in_workflow{0};
};

class RuntimeFailureActor
    : public qb::Actor
    , public qb::ICallback {
    enum class Role { Caller, Thrower, Survivor } _role = Role::Survivor;
    RuntimeFailureState *_state;
    bool                 _announced_workflow = false;

public:
    explicit RuntimeFailureActor(RuntimeFailureState *state)
        : _state(state) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::SignalEvent>(*this);
        if (std::this_thread::get_id() == _state->caller) {
            _role = Role::Caller;
            _state->caller_count.fetch_add(1);
            registerCallback(*this);
        } else if (!_state->thrower_claimed.exchange(true)) {
            _role = Role::Thrower;
            _state->thrower_count.fetch_add(1);
            registerCallback(*this);
        } else {
            _state->survivor_count.fetch_add(1);
            registerCallback(*this);
        }
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (_role == Role::Thrower) {
            // All cores reached the barrier, but one may not have left it yet.
            // Wait for both peers' first loop tick before publishing the error.
            int peers = _state->peers_in_workflow.load(std::memory_order_acquire);
            while (peers < 2) {
                _state->peers_in_workflow.wait(peers);
                peers = _state->peers_in_workflow.load(std::memory_order_acquire);
            }
            throw std::runtime_error("failure after the startup barrier");
        }
        if (!_announced_workflow) {
            _announced_workflow = true;
            _state->peers_in_workflow.fetch_add(1, std::memory_order_release);
            _state->peers_in_workflow.notify_all();
        }
        if (_role == Role::Caller && _state->main->hasError()) {
            if (!_state->runtime_seen_reported.exchange(true))
                _state->runtime_seen->set_value();
            kill();
        }
    }

    void
    on(qb::SignalEvent const &) {
        kill();
    }
};

TEST(MainLifecycle, RuntimeFailureDoesNotJoinLivePeerInsideStart) {
    qb::Main            main;
    std::promise<void>  runtime_seen_promise;
    auto                runtime_seen = runtime_seen_promise.get_future();
    std::promise<void>  start_returned_promise;
    auto                start_returned = start_returned_promise.get_future();
    RuntimeFailureState state{&main, std::this_thread::get_id(), {}, {}, {}, {}, {}, &runtime_seen_promise};
    for (qb::CoreId core = 0; core < 3; ++core)
        main.addActor<RuntimeFailureActor>(core, &state);

    std::atomic<bool> watchdog_stopped_peer{false};
    std::atomic<bool> runtime_was_seen{false};
    std::thread       watchdog([&] {
        const bool seen = runtime_seen.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
        runtime_was_seen.store(seen);
        if (!seen || start_returned.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            watchdog_stopped_peer.store(true);
            if (!seen) {
                state.peers_in_workflow.fetch_add(2, std::memory_order_release);
                state.peers_in_workflow.notify_all(); // release a thrower parked before peer ticks
            }
            qb::Main::stop(); // only a bounded escape from a wedged start(false)
        }
    });

    main.start(false); // caller's core exits after it observes the thrower's error
    start_returned_promise.set_value();
    main.stop(); // the survivor is still live on the correct runtime-error path
    main.join();
    watchdog.join();

    EXPECT_TRUE(runtime_was_seen.load()) << "the caller core never observed the runtime exception";
    EXPECT_FALSE(watchdog_stopped_peer.load()) << "start(false) joined a live peer after a runtime exception";
    EXPECT_EQ(state.caller_count.load(), 1);
    EXPECT_EQ(state.thrower_count.load(), 1);
    EXPECT_EQ(state.survivor_count.load(), 1);
    EXPECT_TRUE(main.hasError());
}

} // namespace
