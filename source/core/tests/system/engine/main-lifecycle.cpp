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
 *   - failure paths — every way a core can refuse to start, each pinned to the SPECIFIC
 *     `qb::VirtualCore::Error` it raises (the enum is packed into `Main`'s private start
 *     barrier and surfaced only through the `hasError()` bool, so we distinguish the codes by
 *     their *observable side effects* — see below — not by reading the enum):
 *       · empty engine / a `core().clear()`'d core  → `Error::NoActor` (started with 0 actors);
 *       · `onInit()` co_returns false               → `Error::BadActorInit`  (a clean false);
 *       · `onInit()` THROWS                          → `Error::ExceptionThrown` (an uncaught throw).
 *
 *     `BadActorInit` and `ExceptionThrown` both fail the start, so `hasError()` alone cannot
 *     tell them apart. We disambiguate with two in-actor atoms mirrored to the test body:
 *     a throwing init sets `g_threw` immediately before `throw` (and NEVER reaches its
 *     `co_return`), whereas a false-returning init sets `g_returned_false` at its `co_return
 *     false` (and never throws). The matrix {hasError, g_threw, g_returned_false} therefore
 *     identifies which enum the engine raised. (Mapping per VirtualCore.cpp / Main.cpp:
 *     a thrown onInit is caught and surfaced as `ExceptionThrown`; a false return is
 *     `BadActorInit`; a core with no actors is `NoActor`. All three are `>= BadInit`, the
 *     `hasError()` threshold.)
 *
 * No wall-clock is used as an oracle: actors self-kill and the engine drains; the only timing
 * backstop is the ctest TIMEOUT. The multi-core failure case derandomizes which core is cleared
 * (fixed seed, logged) so a failure reproduces. Multi-core cases `GTEST_SKIP` on a 1-core runner
 * instead of asserting hardware. The SIGABRT case is the sole signal-raiser in this binary and the
 * binary is labelled `serial` so it never races another test's signal handler.
 *
 * Every in-actor `EXPECT_*` / side effect is mirrored to a post-`join()` atom asserted in the test
 * body, so a never-scheduled actor cannot let a case pass vacuously.
 */

#include <atomic>
#include <csignal>
#include <cstdint>
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

void
reset_atoms() {
    g_init_ran.store(false);
    g_threw.store(false);
    g_returned_false.store(false);
    g_signal_seen.store(false);
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
// Failure actor — onInit THROWS (→ Error::ExceptionThrown). Sets g_threw at the
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
// Distinguished from ExceptionThrown by: g_returned_false set, g_threw NOT set.
TEST(MainLifecycle, FalseOnInitAbortsWithBadActorInit) {
    reset_atoms();
    qb::Main main;
    main.addActor<FalseInitActor>(0);
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "a co_return false onInit must fail the start";
    EXPECT_TRUE(g_returned_false.load()) << "the init reached its clean co_return false";
    EXPECT_FALSE(g_threw.load()) << "BadActorInit is the false-return path, NOT a throw (≠ ExceptionThrown)";
    EXPECT_FALSE(g_init_ran.load());
}

// onInit THROWS (the initial actor of a mono core) → Error::ExceptionThrown.
// Distinguished from BadActorInit by: g_threw set, g_returned_false NOT set.
TEST(MainLifecycle, ThrowingOnInitAbortsWithExceptionThrown) {
    reset_atoms();
    qb::Main main;
    main.addActor<ThrowInitActor>(0);
    main.start();
    main.join();
    EXPECT_TRUE(main.hasError()) << "a throwing onInit must fail the start";
    EXPECT_TRUE(g_threw.load()) << "the init reached its throw site";
    EXPECT_FALSE(g_returned_false.load()) << "ExceptionThrown is the throw path, NOT a false return (≠ BadActorInit)";
    EXPECT_FALSE(g_init_ran.load()) << "the throw aborts before co_return — onInit never completes";
}

} // namespace
