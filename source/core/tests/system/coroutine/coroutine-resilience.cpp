/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-resilience.cpp
 * @brief The resilience combinators on the running engine: `qb::ask_retry` + `qb::ask_guarded`.
 *
 * The pure `CircuitBreaker` state machine is unit-tested in unit/patterns/circuit-breaker.cpp.
 * This is the *engine half*: the two free functions that drive real cross-actor `ask` round-trips
 * under `qb::Main` (qb/core/patterns/resilience.h). Each scenario is proven against framework-
 * observable truth, never wall-clock timing:
 *
 *   ask_retry
 *     - succeeds on the first try (no spurious retry);
 *     - succeeds after N transient timeouts (the flaky dependency eventually answers);
 *     - exhausts its attempts and throws `timeout_error` (and asks EXACTLY max_attempts times);
 *     - GROWS its backoff between attempts — proven by the responder's own VirtualCore arrival
 *       timestamps (gap 2->3 strictly exceeds gap 1->2 under multiplier 2.0), not by a sleep;
 *     - a kill aborts the retry loop with `cancelled_error`.
 *
 *   ask_guarded (CircuitBreaker-protected ask)
 *     - closed: passes every call through, breaker stays closed;
 *     - trips open after the failure threshold, then fails fast with `circuit_open_error`
 *       (EXACT counts: threshold timeouts, the rest open-rejected, breaker ends open);
 *     - half-open recovery: an open breaker admits one trial after cooldown; once the dependency
 *       is healthy that trial succeeds and CLOSES the breaker (the engine recovery path);
 *     - a kill is NOT counted as a breaker failure (failure_count stays 0, breaker stays closed).
 *
 * De-flaking: every in-coroutine `EXPECT_*` is paired with a post-`join()` `g_*_ran` guard so a
 * never-scheduled coroutine cannot pass the test vacuously; shutdown is event-driven (a control
 * event / a counter reached / the shared KillThenStopHelper), and the only timing constants are
 * the centralized retry tunables below — chosen with wide margins, with the ctest TIMEOUT as the
 * sole wall-clock backstop (no tight EXPECT_LT is ever used as an oracle).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites (cross-core /
 * detached asks leave a fixed, benign teardown residual — see system/coroutine/ask-roundtrip.cpp).
 *
 * Promoted from the engine half of the former test-actor-coroutine-resilience.cpp; responders and
 * the kill-then-stop driver are hoisted to shared/AskResponders.h (single source of truth).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/AskResponders.h"

using namespace qb;
using namespace std::chrono_literals;

using qb::test::Echoer;
using qb::test::FlakyMarket;
using qb::test::KillThenStopHelper;
using qb::test::Ping;
using qb::test::SilentMarket;

namespace {

// --- Centralized timing tunables (wide-margin; the ctest TIMEOUT is the real backstop) ---------

/// Per-attempt ask timeout — long enough that a *responding* dependency never spuriously times out,
/// short enough that an exhausting retry sequence stays well under the ctest timeout.
constexpr auto kAskTimeout = 40ms;

/// Deadline for an ask that MUST succeed against a healthy dependency (the half-open recovery
/// trial). Deliberately far larger than @ref kAskTimeout: that one is sized to EXPIRE against a
/// silent peer, which is the opposite requirement. Reusing it made a success-path assertion depend
/// on the machine completing an actor round trip within 40ms — a property of the runner, not of the
/// code under test, and one a loaded Windows CI runner does not hold.
constexpr auto kRecoveryAskTimeout = 2s;

/// First backoff and growth factor. With multiplier 2 and a 320ms cap, the first few inter-attempt
/// gaps are ~20ms, ~40ms, ~80ms — comfortably distinguishable by the VirtualCore clock.
constexpr auto   kBackoff    = 20ms;
constexpr double kMultiplier = 2.0;
constexpr auto   kMaxBackoff = 320ms;

/// Delay before the kill-then-stop driver fires (interrupts an in-flight ask) and then stops.
constexpr auto kKillAfter = 25ms;
constexpr auto kStopAfter = 400ms;

[[nodiscard]] qb::retry_policy
make_policy(int attempts) {
    qb::retry_policy p;
    p.max_attempts = attempts;
    p.backoff      = kBackoff;
    p.multiplier   = kMultiplier;
    p.max_backoff  = kMaxBackoff;
    p.jitter       = 0.0; // deterministic growth: jitter is tested separately (it only shrinks waits)
    return p;
}

} // namespace

// ===========================================================================
// ask_retry
// ===========================================================================

namespace {
std::atomic<int>  g_retry_result{-1};
std::atomic<bool> g_retry_timed_out{false};
std::atomic<bool> g_retry_cancelled{false};
std::atomic<bool> g_retry_ran{false};

void
reset_retry_flags() {
    g_retry_result.store(-1);
    g_retry_timed_out.store(false);
    g_retry_cancelled.store(false);
    g_retry_ran.store(false);
}
} // namespace

// Drives one ask_retry against a target, recording the outcome and a ran-guard, then stops cleanly.
class RetryClient : public qb::Actor {
    qb::ActorId _target;
    int         _attempts;

public:
    RetryClient(qb::ActorId t, int attempts)
        : _target(t)
        , _attempts(attempts) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto t        = _target;
        auto attempts = _attempts;
        spawn([t, attempts](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                auto r = co_await qb::ask_retry(ctx, t, Ping{7}, kAskTimeout, make_policy(attempts));
                g_retry_result.store(r.response);
            } catch (const qb::io::async::timeout_error &) {
                g_retry_timed_out.store(true);
            }
            g_retry_ran.store(true); // proves the coroutine actually reached its end
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskRetry, SucceedsFirstTry) {
    reset_retry_flags();
    qb::Main main;
    auto     echo = main.addActor<Echoer>(0);
    main.addActor<RetryClient>(0, echo, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_ran.load()) << "the retry coroutine must have run to completion";
    EXPECT_EQ(g_retry_result.load(), 14) << "7 * 2, answered on the first attempt";
    EXPECT_FALSE(g_retry_timed_out.load());
}

TEST(ActorAskRetry, SucceedsAfterTransientTimeouts) {
    reset_retry_flags();
    qb::Main main;
    auto     flaky = main.addActor<FlakyMarket>(0, /*reply_on=*/3); // answers only the 3rd attempt
    main.addActor<RetryClient>(0, flaky, 5);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_ran.load());
    EXPECT_EQ(g_retry_result.load(), 14) << "eventually answered after 2 transient timeouts";
    EXPECT_FALSE(g_retry_timed_out.load());
}

TEST(ActorAskRetry, ExhaustsAndThrowsTimeout) {
    reset_retry_flags();
    qb::Main main;
    auto     silent = main.addActor<SilentMarket>(0);
    main.addActor<RetryClient>(0, silent, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_ran.load());
    EXPECT_TRUE(g_retry_timed_out.load()) << "all attempts timed out -> timeout_error propagated";
    EXPECT_EQ(g_retry_result.load(), -1) << "no value was ever produced";
}

// --- Backoff actually GROWS between retries (engine-observable, no sleeps) ------------------------
//
// A FlakyMarket that never answers within the retry budget receives one Ping per attempt and stamps
// each arrival with its VirtualCore clock. Under exponential backoff (multiplier 2) the inter-arrival
// gaps must strictly increase: gap(attempt2->3) > gap(attempt1->2). We assert that ordering (the
// geometric series is deterministic because jitter is 0), not absolute durations — so the test
// proves *growth* without becoming a wall-clock race.

namespace {
std::atomic<bool> g_grow_ran{false};
} // namespace

// Asker that exhausts a multi-attempt retry against a flaky target, then stops once the loop is done.
class BackoffProbeClient : public qb::Actor {
    qb::ActorId _target;
    int         _attempts;

public:
    BackoffProbeClient(qb::ActorId t, int attempts)
        : _target(t)
        , _attempts(attempts) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto t        = _target;
        auto attempts = _attempts;
        spawn([t, attempts](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_retry(ctx, t, Ping{1}, kAskTimeout, make_policy(attempts));
            } catch (const qb::io::async::timeout_error &) {
                // expected: the target never answers within the budget
            }
            g_grow_ran.store(true);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskRetry, BackoffGrowsBetweenAttempts) {
    g_grow_ran.store(false);
    // Caller-owned timeline; the responder fills it on core 0, we read it after join() (happens-before).
    FlakyMarket::ArrivalLog log;
    qb::Main                main;
    // reply_on far beyond max_attempts -> never answers; we get one arrival per attempt.
    auto flaky = main.addActor<FlakyMarket>(0, /*reply_on=*/100, &log);
    main.addActor<BackoffProbeClient>(0, flaky, /*attempts=*/4); // 4 arrivals -> 3 gaps
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_grow_ran.load());
    EXPECT_EQ(log.count(), 4u) << "ask_retry must send exactly max_attempts requests";

    const auto gaps = log.gaps();
    ASSERT_EQ(gaps.size(), 3u) << "4 attempts -> 3 inter-arrival gaps";
    // The first gap includes ~one ask timeout + the first backoff; the next gap adds the GROWN
    // backoff. Each successive gap must be strictly larger (multiplier 2, no jitter).
    EXPECT_GT(gaps[1], gaps[0]) << "backoff between attempt 2->3 must exceed 1->2 (it grew)";
    EXPECT_GT(gaps[2], gaps[1]) << "backoff between attempt 3->4 must exceed 2->3 (it grew again)";
}

// --- A kill aborts the retry loop with cancelled_error -------------------------------------------

namespace {
std::atomic<bool> g_retry_cancel_ran{false};
} // namespace

// Retries forever against a silent target; the shared KillThenStopHelper kills it mid-wait.
class RetryCancelClient : public qb::Actor {
    qb::ActorId _target;

public:
    explicit RetryCancelClient(qb::ActorId t)
        : _target(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto t = _target;
        spawn([t](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                // A long per-attempt timeout + many attempts: we are killed long before it exhausts.
                co_await qb::ask_retry(ctx, t, Ping{1}, 100ms, make_policy(50));
            } catch (const qb::io::async::cancelled_error &) {
                g_retry_cancelled.store(true);
            }
            g_retry_cancel_ran.store(true);
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskRetry, CancelledOnKillAbortsRetries) {
    g_retry_cancelled.store(false);
    g_retry_cancel_ran.store(false);
    qb::Main main;
    auto     silent = main.addActor<SilentMarket>(0);
    auto     client = main.addActor<RetryCancelClient>(0, silent);
    main.addActor<KillThenStopHelper<>>(0, client, kKillAfter, kStopAfter);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_retry_cancel_ran.load()) << "the retry coroutine must have unwound";
    EXPECT_TRUE(g_retry_cancelled.load()) << "the kill must surface as cancelled_error, aborting retries";
}

// ===========================================================================
// ask_guarded (CircuitBreaker-protected ask)
// ===========================================================================

namespace {
std::atomic<int>  g_guard_success{0};
std::atomic<int>  g_guard_timeout{0};
std::atomic<int>  g_guard_open{0};
std::atomic<bool> g_guard_cancelled{false};
std::atomic<bool> g_guard_ran{false};

void
reset_guard_flags() {
    g_guard_success.store(0);
    g_guard_timeout.store(0);
    g_guard_open.store(0);
    g_guard_cancelled.store(false);
    g_guard_ran.store(false);
}

using State = qb::CircuitBreaker::State;
} // namespace

// Issues N guarded asks in a loop, tallying success / open-rejection / timeout, then stops.
class GuardClient : public qb::Actor {
    std::shared_ptr<qb::CircuitBreaker> _breaker;
    qb::ActorId                         _target;
    int                                 _calls;

public:
    GuardClient(std::shared_ptr<qb::CircuitBreaker> b, qb::ActorId t, int calls)
        : _breaker(std::move(b))
        , _target(t)
        , _calls(calls) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto b = _breaker;
        auto t = _target;
        auto n = _calls;
        spawn([b, t, n](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < n; ++i) {
                try {
                    auto r = co_await qb::ask_guarded(ctx, b, t, Ping{i}, kAskTimeout);
                    (void) r;
                    g_guard_success.fetch_add(1);
                } catch (const qb::circuit_open_error &) {
                    g_guard_open.fetch_add(1);
                } catch (const qb::io::async::timeout_error &) {
                    g_guard_timeout.fetch_add(1);
                }
            }
            g_guard_ran.store(true);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskGuarded, ClosedPassesThrough) {
    reset_guard_flags();
    auto     breaker = std::make_shared<qb::CircuitBreaker>(2u, 10s);
    qb::Main main;
    auto     echo = main.addActor<Echoer>(0);
    main.addActor<GuardClient>(0, breaker, echo, 3);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_guard_ran.load());
    EXPECT_EQ(g_guard_success.load(), 3) << "every closed-breaker call passes through";
    EXPECT_EQ(g_guard_open.load(), 0);
    EXPECT_EQ(g_guard_timeout.load(), 0);
    EXPECT_EQ(breaker->state(), State::closed);
    EXPECT_EQ(breaker->failure_count(), 0u);
}

TEST(ActorAskGuarded, TripsOpenThenFailsFast) {
    reset_guard_flags();
    auto     breaker = std::make_shared<qb::CircuitBreaker>(2u, 10s); // opens after 2 failures
    qb::Main main;
    auto     silent = main.addActor<SilentMarket>(0);
    main.addActor<GuardClient>(0, breaker, silent, 4); // 2 timeouts trip it, next 2 fail fast
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_guard_ran.load());
    EXPECT_EQ(g_guard_success.load(), 0);
    EXPECT_EQ(g_guard_timeout.load(), 2) << "first two attempts actually time out (request sent)";
    EXPECT_EQ(g_guard_open.load(), 2) << "remaining two fail fast with circuit_open_error (no request)";
    EXPECT_EQ(breaker->state(), State::open);
}

// --- NEW: half-open -> recovery path on the running engine ---------------------------------------
//
// A breaker with a SHORT cooldown is tripped open by real timeouts against a silent dependency;
// once the cooldown elapses it admits one half-open trial, and because the dependency is now
// healthy that trial SUCCEEDS and closes the breaker. The oracle is the breaker's own observable
// state after join() (closed) plus an explicit "the recovery call returned a real value", not any
// wall-clock measurement. A FlakyMarket that answers from the (threshold+1)-th request onward is
// the dependency: the first `threshold` requests are dropped (tripping the breaker), and the
// half-open trial after cooldown is answered.

namespace {
std::atomic<int>  g_recover_success{0};
std::atomic<int>  g_recover_timeout{0};
std::atomic<int>  g_recover_open{0};
std::atomic<int>  g_recover_value{-1};
std::atomic<bool> g_recover_ran{false};
} // namespace

// Repeatedly guarded-asks the same target; sleeps a cooldown-spanning interval between bursts so the
// open breaker gets a chance to half-open and recover once the dependency is answering.
class RecoveryClient : public qb::Actor {
    std::shared_ptr<qb::CircuitBreaker> _breaker;
    qb::ActorId                         _target;
    unsigned                            _threshold;
    qb::duration                        _cooldown;

public:
    RecoveryClient(std::shared_ptr<qb::CircuitBreaker> b, qb::ActorId t, unsigned threshold, qb::duration cooldown)
        : _breaker(std::move(b))
        , _target(t)
        , _threshold(threshold)
        , _cooldown(cooldown) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto b         = _breaker;
        auto t         = _target;
        auto threshold = _threshold;
        auto cooldown  = _cooldown;
        spawn([b, t, threshold, cooldown](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            // Phase 1: drive `threshold` failures to trip the breaker open.
            for (unsigned i = 0; i < threshold; ++i) {
                try {
                    co_await qb::ask_guarded(ctx, b, t, Ping{static_cast<int>(i)}, kAskTimeout);
                    g_recover_success.fetch_add(1);
                } catch (const qb::circuit_open_error &) {
                    g_recover_open.fetch_add(1);
                } catch (const qb::io::async::timeout_error &) {
                    g_recover_timeout.fetch_add(1);
                }
            }
            // The breaker must be open now (event-observable, not timing).
            EXPECT_EQ(b->state(), State::open) << "threshold timeouts must have tripped the breaker open";

            // Phase 2: wait past the cooldown (cancellation-aware), then a single guarded ask must
            // be admitted as the half-open trial and, with the dependency now healthy, succeed and
            // close the breaker.
            co_await ctx.sleep(cooldown + 50ms);
            try {
                // A GENEROUS deadline here, deliberately unlike phase 1. The two phases want
                // opposite things from the same constant: phase 1 needs `kAskTimeout` (40ms) to
                // EXPIRE against a silent dependency, which is what trips the breaker; phase 2 needs
                // this ask to SUCCEED against a healthy one. Reusing 40ms made phase 2 assert that
                // the machine completes an actor round trip within 40ms -- a property of the runner,
                // not of the circuit breaker. On a loaded Windows CI runner it does not, and the
                // trial timed out with the dependency answering normally (observed:
                // g_recover_value == -1 instead of 1554). What this phase actually pins is that an
                // open breaker ADMITS one trial after the cooldown and closes on its success; the
                // deadline only has to be long enough not to be the thing under test.
                auto r = co_await qb::ask_guarded(ctx, b, t, Ping{777}, kRecoveryAskTimeout);
                g_recover_value.store(r.response);
                g_recover_success.fetch_add(1);
            } catch (const qb::circuit_open_error &) {
                g_recover_open.fetch_add(1);
            } catch (const qb::io::async::timeout_error &) {
                g_recover_timeout.fetch_add(1);
            }
            g_recover_ran.store(true);
            qb::Main::stop();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskGuarded, HalfOpenTrialRecoversAndCloses) {
    g_recover_success.store(0);
    g_recover_timeout.store(0);
    g_recover_open.store(0);
    g_recover_value.store(-1);
    g_recover_ran.store(false);

    constexpr unsigned threshold = 2u;
    constexpr auto     cooldown  = 60ms;
    auto               breaker   = std::make_shared<qb::CircuitBreaker>(threshold, cooldown);

    qb::Main main;
    // Drops the first `threshold` requests (tripping the breaker), answers from the next on.
    auto flaky = main.addActor<FlakyMarket>(0, /*reply_on=*/static_cast<int>(threshold) + 1);
    main.addActor<RecoveryClient>(0, breaker, flaky, threshold, cooldown);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_recover_ran.load()) << "the recovery coroutine must have run to completion";
    EXPECT_EQ(g_recover_timeout.load(), static_cast<int>(threshold)) << "exactly `threshold` real timeouts trip it";
    EXPECT_EQ(g_recover_value.load(), 777 * 2) << "the half-open trial returned the dependency's real reply";
    EXPECT_EQ(g_recover_success.load(), 1) << "only the post-cooldown trial succeeded";
    EXPECT_EQ(breaker->state(), State::closed) << "a successful half-open trial closes the breaker";
    EXPECT_EQ(breaker->failure_count(), 0u);
}

// --- A kill is NOT counted as a breaker failure --------------------------------------------------

namespace {
std::atomic<bool> g_guard_cancel_ran{false};
} // namespace

// Single guarded ask against a silent target with a long timeout; the shared driver kills it mid-wait.
class GuardCancelClient : public qb::Actor {
    std::shared_ptr<qb::CircuitBreaker> _breaker;
    qb::ActorId                         _target;

public:
    GuardCancelClient(std::shared_ptr<qb::CircuitBreaker> b, qb::ActorId t)
        : _breaker(std::move(b))
        , _target(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        auto b = _breaker;
        auto t = _target;
        spawn([b, t](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask_guarded(ctx, b, t, Ping{0}, 500ms); // long wait -> killed first
            } catch (const qb::io::async::cancelled_error &) {
                g_guard_cancelled.store(true);
            }
            g_guard_cancel_ran.store(true);
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

TEST(ActorAskGuarded, CancelledIsNotCountedAsFailure) {
    g_guard_cancelled.store(false);
    g_guard_cancel_ran.store(false);
    auto     breaker = std::make_shared<qb::CircuitBreaker>(5u, 10s);
    qb::Main main;
    auto     silent = main.addActor<SilentMarket>(0);
    auto     client = main.addActor<GuardCancelClient>(0, breaker, silent);
    main.addActor<KillThenStopHelper<>>(0, client, kKillAfter, kStopAfter);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_guard_cancel_ran.load()) << "the guarded coroutine must have unwound";
    EXPECT_TRUE(g_guard_cancelled.load()) << "the kill surfaces as cancelled_error";
    EXPECT_EQ(breaker->failure_count(), 0u) << "a kill must NOT count as a breaker failure";
    EXPECT_EQ(breaker->state(), State::closed) << "the breaker stays closed after a cancelled trial";
}
