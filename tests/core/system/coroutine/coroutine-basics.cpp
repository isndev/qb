/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-basics.cpp
 * @brief The basic actor-coroutine round-trip on the live event loop.
 *
 * This is the smoke/basics layer beneath `coroutine-detached.cpp` (the broad `spawn_detached`
 * behavioural sweep) and `coroutine-scope.cpp` (the cancellation contract). It proves the minimal
 * coroutine lifecycle an actor relies on:
 *   spawn_detached(lambda → task<void>) → co_await sleep → ctx.push<E>() → on(E) handler.
 * plus the safe `CoroContext` surface (`ctx.id()`, `ctx.time()`, `ctx.push`), multiple concurrent
 * coroutines per actor, a locally-caught exception inside a coroutine body, and the two teardown
 * shapes a real actor must survive: dying while a coroutine is still parked, and a coroutine that
 * lets an exception ESCAPE its frame (the engine must absorb it, not terminate).
 *
 * Every in-coroutine / in-handler effect is mirrored to a file-scope `std::atomic` that is reset
 * before the run and asserted AFTER `join()` — so a coroutine that silently never delivered fails
 * the assertion instead of hanging (no pass-if-never-run). Where a test needs a "should never run"
 * sentinel, it parks a scoped coroutine on `ctx.until_cancelled()` (cut short by kill, no wall-clock
 * magic number) and asserts the body never completed — instead of encoding a 10s sleep as the
 * sentinel.
 *
 * Shutdown is deterministic and event-driven: the actor under test calls `qb::Main::stop()` the
 * instant it observes completion, tearing the engine down immediately. A LOUD bounded watchdog
 * actor co-runs purely as a backstop — it stops the engine and flips `g_watchdog_fired` only if the
 * deadline is reached first, so a hung run FAILS with a message (`g_watchdog_fired == true`) instead
 * of relying on the ctest TIMEOUT.
 */

#include <atomic>
#include <chrono>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Per-run observation flags (reset at the start of every TEST).
// ---------------------------------------------------------------------------
std::atomic<int>  g_basic_result{-1};          // value the BasicCoroActor handler observed
std::atomic<int>  g_multi_completed{0};        // increments delivered to MultiCoroActor
std::atomic<bool> g_exc_caught{false};         // local catch inside ExceptionCoroActor ran
std::atomic<int>  g_safety_result{-1};         // value the SafetyTestActor handler observed
std::atomic<bool> g_safety_ctx_valid{false};   // ctx.id().is_valid() && ctx.time() > 0 held
std::atomic<bool> g_quickkill_body_ran{false}; // the parked coro body MUST NOT complete
std::atomic<bool> g_escaping_threw{false};     // the escaping coroutine reached its throw
std::atomic<int>  g_survivor_result{-1};       // sibling coroutine that must still deliver

// A LOUD bounded watchdog: stops the engine after `deadline` and flips `g_watchdog_fired`.
// A test asserts `g_watchdog_fired == false` after join() — a hung run fails with a message
// instead of relying on the ctest TIMEOUT.
std::atomic<bool> g_watchdog_fired{false};

void
reset_flags() {
    g_basic_result       = -1;
    g_multi_completed    = 0;
    g_exc_caught         = false;
    g_safety_result      = -1;
    g_safety_ctx_valid   = false;
    g_quickkill_body_ran = false;
    g_escaping_threw     = false;
    g_survivor_result    = -1;
    g_watchdog_fired     = false;
}

} // namespace

// Bounded deadline backstop: stops the engine and records that it had to, so a test that
// would otherwise hang fails loudly instead. Add it alongside the actor under test; the
// happy path always finishes (and stops the engine) well before the deadline.
class WatchdogActor : public qb::Actor {
    const qb::duration _deadline;

public:
    explicit WatchdogActor(qb::duration deadline)
        : _deadline(deadline) {}

    qb::io::async::task<bool>
    onInit() override {
        qb::io::async::callback(
            [] {
                g_watchdog_fired.store(true, std::memory_order_relaxed);
                qb::Main::stop();
            },
            _deadline);
        co_return true;
    }
};

// Shared events.
struct CoroCompletedEvent : public qb::Event {
    int result{0};
    CoroCompletedEvent() = default;
    explicit CoroCompletedEvent(int r)
        : result(r) {}
};
struct CoroIncrementEvent : public qb::Event {};

// ---------------------------------------------------------------------------
// 1. Basic spawn → sleep → push → deliver → handler round-trip.
// ---------------------------------------------------------------------------
class BasicCoroActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(10ms);
            ctx.template push<CoroCompletedEvent>(42);
        });
        co_return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        g_basic_result.store(ev.result, std::memory_order_relaxed);
        qb::Main::stop(); // deterministic, immediate engine shutdown on success
    }
};

TEST(ActorCoroutine, BasicCoroutineExecution) {
    reset_flags();
    qb::Main main;
    main.addActor<BasicCoroActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_watchdog_fired.load()) << "the coroutine round-trip did not complete in time";
    EXPECT_EQ(g_basic_result.load(), 42) << "ctx.push payload must round-trip exactly";
}

// ---------------------------------------------------------------------------
// 2. Several concurrent detached coroutines per actor all deliver.
// ---------------------------------------------------------------------------
class MultiCoroActor : public qb::Actor {
    static constexpr int EXPECTED_COUNT = 5;
    int                  completed_{0};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroIncrementEvent>(*this);
        for (int i = 0; i < EXPECTED_COUNT; ++i) {
            spawn_detached([i](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(std::chrono::milliseconds(5 * (i + 1)));
                ctx.template push<CoroIncrementEvent>();
            });
        }
        co_return true;
    }

    void
    on(const CoroIncrementEvent &) {
        ++completed_;
        g_multi_completed.fetch_add(1, std::memory_order_relaxed);
        if (completed_ >= EXPECTED_COUNT)
            qb::Main::stop();
    }
};

TEST(ActorCoroutine, MultipleCoroutinesPerActor) {
    reset_flags();
    qb::Main main;
    main.addActor<MultiCoroActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_watchdog_fired.load()) << "not every concurrent coroutine delivered in time";
    EXPECT_EQ(g_multi_completed.load(), 5) << "exactly five concurrent coroutines must each deliver once";
}

// ---------------------------------------------------------------------------
// 3. An exception thrown and caught INSIDE the coroutine body does not break
//    the loop; the local catch path runs and a completion event is pushed.
// ---------------------------------------------------------------------------
class ExceptionCoroActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::io::async::sleep(5ms);
                throw std::runtime_error("Test exception");
            } catch (const std::exception &) {
                g_exc_caught.store(true, std::memory_order_relaxed);
                ctx.template push<CoroCompletedEvent>(1);
            }
        });
        co_return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        g_basic_result.store(ev.result, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutine, ExceptionHandledInsideCoroutine) {
    reset_flags();
    qb::Main main;
    main.addActor<ExceptionCoroActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_watchdog_fired.load()) << "the locally-caught coroutine did not complete in time";
    EXPECT_TRUE(g_exc_caught.load()) << "the in-coroutine catch block must have run";
    EXPECT_EQ(g_basic_result.load(), 1) << "the catch path must push the completion event";
}

// ---------------------------------------------------------------------------
// 4. CoroContext safe surface: ctx.id() is valid and ctx.time() advances; the
//    only legal mutation (push) round-trips a value back to the actor.
// ---------------------------------------------------------------------------
class SafetyTestActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            const bool ok = ctx.id().is_valid() && ctx.time() > 0;
            g_safety_ctx_valid.store(ok, std::memory_order_relaxed);
            co_await qb::io::async::sleep(5ms);
            ctx.template push<CoroCompletedEvent>(99);
        });
        co_return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        g_safety_result.store(ev.result, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutine, CoroContextSafety) {
    reset_flags();
    qb::Main main;
    main.addActor<SafetyTestActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_watchdog_fired.load()) << "the CoroContext safety coroutine did not complete in time";
    EXPECT_TRUE(g_safety_ctx_valid.load()) << "ctx.id() must be valid and ctx.time() must be > 0";
    EXPECT_EQ(g_safety_result.load(), 99) << "the safe ctx.push surface must round-trip the value";
}

// ---------------------------------------------------------------------------
// 5. Actor dies while a coroutine is still parked — no crash / no leak, and the
//    parked body MUST NOT run. We park a SCOPED coroutine on until_cancelled()
//    (cut short by kill — no wall-clock magic sentinel) and assert post-join the
//    body never completed. A keeper holds the engine open briefly to give a
//    (buggy) resumption a chance to flip the flag, so the assertion has teeth.
// ---------------------------------------------------------------------------
class QuickKillActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.until_cancelled();                              // parks; cancelled (not resumed) when we die
            g_quickkill_body_ran.store(true, std::memory_order_relaxed); // MUST NOT run
        });
        // Die immediately, while the coroutine is parked.
        kill();
        co_return true;
    }
};

TEST(ActorCoroutine, ActorDiesWithParkedCoroutine) {
    reset_flags();
    qb::Main main;
    main.addActor<QuickKillActor>(0);
    main.addActor<WatchdogActor>(0, 200ms); // keeps the loop alive past any spurious resume, then stops
    main.start(false);
    main.join();
    // The watchdog firing here is the EXPECTED, designed shutdown (QuickKillActor never stops the
    // engine), so we do NOT assert on it — only that the parked body stayed parked.
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_quickkill_body_ran.load()) << "a coroutine parked on until_cancelled() must be cancelled, not resumed, when its actor dies";
}

// ---------------------------------------------------------------------------
// 6. An UNCAUGHT exception escaping a coroutine frame is absorbed by the engine
//    (stored in the promise, frame reclaimed) — it must NOT terminate the
//    process or set hasError(), and sibling coroutines on the same actor still
//    run to completion. This is the case the old "exception" test pretended to
//    cover but did not (it only exercised the locally-caught path).
// ---------------------------------------------------------------------------
class EscapingExceptionActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroCompletedEvent>(*this);
        // Coroutine A: lets a runtime_error ESCAPE its frame (no try/catch).
        spawn_detached([](auto) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            g_escaping_threw.store(true, std::memory_order_relaxed);
            throw std::runtime_error("escapes the coroutine frame");
        });
        // Coroutine B: an independent sibling that must still deliver, proving the
        // escaping exception did not poison the scheduler / the actor.
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(10ms);
            ctx.template push<CoroCompletedEvent>(7);
        });
        co_return true;
    }

    void
    on(const CoroCompletedEvent &ev) {
        g_survivor_result.store(ev.result, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutine, UncaughtExceptionEscapingCoroutineIsAbsorbed) {
    reset_flags();
    qb::Main main;
    main.addActor<EscapingExceptionActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError()) << "an uncaught coroutine exception must not surface as an engine error";
    EXPECT_FALSE(g_watchdog_fired.load()) << "the surviving sibling coroutine did not complete in time";
    EXPECT_TRUE(g_escaping_threw.load()) << "the escaping coroutine must have reached its throw";
    EXPECT_EQ(g_survivor_result.load(), 7) << "a sibling coroutine must still run after another coroutine threw uncaught";
}
