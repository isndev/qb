/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-detached.cpp
 * @brief The `spawn_detached` ("orphan-and-complete") behavioural sweep on the live event loop.
 *
 * `spawn_detached(lambda → task<void>)` launches a coroutine bound to the *core scheduler*, not the
 * actor scope: it is NOT cancelled when the actor dies (that is `spawn()`, covered by
 * coroutine-scope.cpp) — it runs to completion and the scheduler reclaims the frame. This suite
 * exercises that path and the `CoroContext` surface (`ctx.push` / `push_to` / `id`), capture
 * survival across suspensions, the awaiter/combinator library (`sleep`, `when_all`,
 * `coro_with_timeout`), exception propagation through chained `co_await`, the
 * `active_coroutine_count()` RAII counter, and coroutine lifetime vs actor death.
 *
 * Hardening over the original `-advanced` harness (see docs/tests-audit/qb-core/qbcore-c09.md):
 *   - DE-FLAKED: completion is driven by event delivery, not by hard-coded wall-clock ordering, and
 *     the engine is torn down by `qb::Main::stop()` the instant the result is observed. A LOUD
 *     bounded `WatchdogActor` co-runs as a backstop: it stops the engine and flips
 *     `g_watchdog_fired` only if its deadline is reached first, so a hung / reordered run FAILS with
 *     a message (`g_watchdog_fired == true`) instead of relying on the ctest TIMEOUT. The previously
 *     racy orphan-vs-keepalive case (`CoroutineOutlivesActor`) now gates the keeper's shutdown on
 *     the orphan actually signalling completion (an event), not on `30ms < 200ms`.
 *   - PRUNED VACUOUS: the in-coroutine `EXPECT_EQ(magic, 0xCAFE)` self-captured-constant checks (a
 *     compiler coroutine-frame-capture test that ASan, not EXPECT, guards) are dropped; ONE capture
 *     witness is kept (`MixedTypeCapturesSurvive`) and the load-bearing assertion in every capture
 *     test is the post-`push` handler value check, which genuinely traversed the framework.
 *   - FIXED the WEAK counter test: `CounterAccuracy` now READS `active_coroutine_count()` and proves
 *     it rose to N at spawn and fell back to 0 after completion (the RAII decrement), instead of
 *     setting an unconditional bool.
 *
 * Every in-coroutine / in-handler effect is mirrored to a file-scope atomic asserted AFTER `join()`
 * (no pass-if-never-run).
 */

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// LOUD bounded deadline backstop. Co-run alongside the actor(s) under test: on the
// happy path the actor calls qb::Main::stop() the instant it observes completion,
// long before the deadline; if it hangs, the watchdog stops the engine and sets
// g_watchdog_fired, which the test asserts is false — a hung run fails with a
// message instead of relying on the ctest TIMEOUT.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_watchdog_fired{false};

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

// Helper: a TEST asserts the watchdog did not have to fire (completion was observed in time).
#define EXPECT_NO_WATCHDOG() EXPECT_FALSE(g_watchdog_fired.load()) << "engine hit the bounded deadline — a coroutine never completed"

// ---------------------------------------------------------------------------
// Shared test events
// ---------------------------------------------------------------------------

struct DoneEvent : qb::Event {};

struct ValueEvent : qb::Event {
    int value{0};
    ValueEvent() = default;
    explicit ValueEvent(int v)
        : value(v) {}
};

struct StringEvent : qb::Event {
    std::array<char, 128> payload{};
    int                   length{0};
    StringEvent() = default;
    explicit StringEvent(std::string_view s)
        : length(static_cast<int>(s.size())) {
        std::copy_n(s.begin(), std::min(s.size(), payload.size()), payload.begin());
    }
    [[nodiscard]] std::string_view
    str() const {
        return {payload.data(), static_cast<std::size_t>(length)};
    }
};

struct CoroPingEvent : qb::Event {
    ActorId sender;
    int     seq{0};
    CoroPingEvent() = default;
    CoroPingEvent(ActorId s, int sq)
        : sender(s)
        , seq(sq) {}
};

struct CoroPongEvent : qb::Event {
    int seq{0};
    CoroPongEvent() = default;
    explicit CoroPongEvent(int sq)
        : seq(sq) {}
};

// ===========================================================================
// 1. Lambda capture safety: captured data survives multiple suspensions.
//    (Pruned: the in-coroutine constant EXPECTs were a compiler capture test —
//    ASan guards that. The teeth here are the byte-exact payload in the handler.)
// ===========================================================================

static std::atomic<int> g_captured_len{-1};

class CapturedDataSurvivesActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StringEvent>(*this);
        std::string big_data(64, 'A');
        spawn_detached([big_data](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms); // 1st suspension
            co_await qb::io::async::sleep(5ms); // 2nd suspension
            co_await qb::io::async::sleep(5ms); // 3rd suspension
            ctx.template push<StringEvent>(big_data);
        });
        co_return true;
    }

    void
    on(const StringEvent &ev) {
        EXPECT_EQ(ev.str(), std::string(64, 'A')); // teeth: survived 3 suspensions, byte-exact
        g_captured_len.store(ev.length, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, CapturedDataSurvivesMultipleSuspensions) {
    g_captured_len   = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<CapturedDataSurvivesActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_captured_len.load(), 64) << "the captured 64-byte payload must survive all suspensions";
}

// ===========================================================================
// 2. Capture a non-trivial vector by value — survives suspensions.
//    (Pruned in-coroutine constant EXPECTs; the handler sum is the real oracle.)
// ===========================================================================

static std::atomic<int> g_vec_sum{-1};

class VectorCaptureActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        std::vector<int> data = {10, 20, 30, 40, 50};
        spawn_detached([data](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            int sum = 0;
            for (auto v : data)
                sum += v;
            co_await qb::io::async::sleep(5ms);
            ctx.template push<ValueEvent>(sum);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 150); // teeth: 10+20+30+40+50 computed from the captured vector
        g_vec_sum.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, NonTrivialCaptureVector) {
    g_vec_sum        = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<VectorCaptureActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_vec_sum.load(), 150) << "captured vector must survive suspension and sum exactly";
}

// ===========================================================================
// 3. Counter accuracy: active_coroutine_count() rises to N on spawn and falls
//    back to 0 after the coroutines complete (the RAII decrement). The fix over
//    the old WEAK version: it now READS the counter at both points instead of
//    setting an unconditional bool.
// ===========================================================================

static std::atomic<int> g_count_at_spawn{-1}; // active_coroutine_count() right after spawning N
static std::atomic<int> g_count_after{-1};    // active_coroutine_count() once all N completed

class CounterAccuracyActor : public qb::Actor {
    int                  completed_{0};
    static constexpr int NUM_COROS = 3;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        EXPECT_EQ(active_coroutine_count(), 0u) << "no coroutines before the first spawn";
        for (int i = 0; i < NUM_COROS; ++i) {
            spawn_detached([](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(10ms);
                ctx.template push<DoneEvent>();
            });
        }
        g_count_at_spawn.store(static_cast<int>(active_coroutine_count()), std::memory_order_relaxed);
        co_return true;
    }

    void
    on(const DoneEvent &) {
        if (++completed_ < NUM_COROS)
            return;
        // All NUM_COROS coroutines have reached final_suspend and their RAII guards have decremented
        // the counter. We are running inside the last DoneEvent handler — the 3 originals are gone
        // and we have not spawned anything new, so the live count must be back to 0.
        g_count_after.store(static_cast<int>(active_coroutine_count()), std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, CounterAccuracy) {
    g_count_at_spawn = -1;
    g_count_after    = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<CounterAccuracyActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_count_at_spawn.load(), 3) << "active_coroutine_count() must equal the number spawned";
    EXPECT_EQ(g_count_after.load(), 0) << "the RAII guard must decrement the counter back to 0 on completion";
}

// ===========================================================================
// 4. Chained sub-tasks: co_await a helper task inside spawn_detached.
// ===========================================================================

namespace {
qb::io::async::task<int>
async_compute(int a, int b) {
    co_await qb::io::async::sleep(5ms);
    co_return a + b;
}

qb::io::async::task<int>
async_pipeline(int x) {
    int step1 = co_await async_compute(x, 10);
    int step2 = co_await async_compute(step1, 20);
    co_return step2;
}
} // namespace

static std::atomic<int> g_chain_result{-1};

class ChainedTaskActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        int base = 5;
        spawn_detached([base](auto ctx) -> qb::io::async::task<void> {
            int result = co_await async_pipeline(base); // 5+10=15, 15+20=35
            ctx.template push<ValueEvent>(result);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 35);
        g_chain_result.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, ChainedSubTasks) {
    g_chain_result   = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<ChainedTaskActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_chain_result.load(), 35) << "chained co_await must compose: (5+10)+20";
}

// ===========================================================================
// 5. CoroContext::push_to — send event to a different actor.
// ===========================================================================

static std::atomic<int> g_pushto_value{-1};

class ReceiverActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 777);
        g_pushto_value.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

class SenderCoroActor : public qb::Actor {
    ActorId target_;

public:
    explicit SenderCoroActor(ActorId target)
        : target_(target) {}

    qb::io::async::task<bool>
    onInit() override {
        ActorId dest = target_;
        spawn_detached([dest](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(10ms);
            ctx.template push_to<ValueEvent>(dest, 777);
        });
        co_return true;
    }
};

TEST(ActorCoroutineAdvanced, PushToOtherActor) {
    g_pushto_value   = -1;
    g_watchdog_fired = false;
    qb::Main main;

    auto receiver_id = main.addActor<ReceiverActor>(0);
    ASSERT_NE(receiver_id, ActorId::NotFound);
    main.addActor<SenderCoroActor>(0, receiver_id);
    main.addActor<WatchdogActor>(0, 5s);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_pushto_value.load(), 777) << "ctx.push_to must deliver to the targeted actor";
}

// ===========================================================================
// 6. Rapid spawn/complete stress — many short-lived coroutines all deliver.
// ===========================================================================

static std::atomic<int> g_stress_completed{0};

class StressSpawnActor : public qb::Actor {
    static constexpr int TOTAL = 50;
    int                  completed_{0};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        for (int i = 0; i < TOTAL; ++i) {
            spawn_detached([i](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(std::chrono::milliseconds(1 + (i % 5)));
                ctx.template push<DoneEvent>();
            });
        }
        co_return true;
    }

    void
    on(const DoneEvent &) {
        ++completed_;
        g_stress_completed.fetch_add(1, std::memory_order_relaxed);
        if (completed_ >= TOTAL)
            qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, RapidSpawnStress) {
    g_stress_completed = 0;
    g_watchdog_fired   = false;
    qb::Main main;
    main.addActor<StressSpawnActor>(0);
    main.addActor<WatchdogActor>(0, 10s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_stress_completed.load(), 50) << "every one of the 50 rapid coroutines must deliver exactly once";
}

// ===========================================================================
// 7. Actor killed while coroutines are pending — no crash / UB. The counter is
//    read at spawn (5 pending) to give the assertion teeth.
// ===========================================================================

static std::atomic<int> g_earlydeath_count{-1};

class EarlyDeathActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 5; ++i) {
            spawn_detached([](auto) -> qb::io::async::task<void> { co_await qb::io::async::sleep(500ms); });
        }
        EXPECT_TRUE(has_active_coroutines());
        g_earlydeath_count.store(static_cast<int>(active_coroutine_count()), std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

TEST(ActorCoroutineAdvanced, ActorDiesWithPendingCoroutines) {
    g_earlydeath_count = -1;
    g_watchdog_fired   = false;
    qb::Main main;
    main.addActor<EarlyDeathActor>(0);
    // No watchdog needed: the only actor self-kills in onInit and the 500ms detached coroutines are
    // orphaned, so the engine drains and exits on its own. This is the no-crash/no-leak smoke.
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_earlydeath_count.load(), 5) << "all 5 spawned coroutines must be counted before the actor dies";
}

// ===========================================================================
// 8. Two-actor coroutine ping-pong via events.
// ===========================================================================

static std::atomic<int> g_pong_count{0};

class PongActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroPingEvent>(*this);
        co_return true;
    }

    void
    on(const CoroPingEvent &ev) {
        g_pong_count.fetch_add(1, std::memory_order_relaxed);
        push<CoroPongEvent>(ev.sender, ev.seq);
    }
};

class PingCoroActor : public qb::Actor {
    ActorId              pong_;
    int                  received_{0};
    static constexpr int ROUNDS = 5;

public:
    explicit PingCoroActor(ActorId pong)
        : pong_(pong) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CoroPongEvent>(*this);
        ActorId dest = pong_;
        for (int i = 0; i < ROUNDS; ++i) {
            spawn_detached([dest, i](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(std::chrono::milliseconds(5 * (i + 1)));
                ctx.template push_to<CoroPingEvent>(dest, ctx.id(), i);
            });
        }
        co_return true;
    }

    void
    on(const CoroPongEvent &ev) {
        EXPECT_GE(ev.seq, 0);
        EXPECT_LT(ev.seq, ROUNDS);
        if (++received_ >= ROUNDS)
            qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, TwoActorPingPong) {
    g_pong_count     = 0;
    g_watchdog_fired = false;
    qb::Main main;

    auto pong_id = main.addActor<PongActor>(0);
    ASSERT_NE(pong_id, ActorId::NotFound);
    main.addActor<PingCoroActor>(0, pong_id);
    main.addActor<WatchdogActor>(0, 5s);

    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_pong_count.load(), 5) << "exactly 5 ping/pong rounds must complete";
}

// ===========================================================================
// 9. Exception propagation inside chained co_await: an inner task throws and the
//    outer coroutine's catch sees it (the awaiter re-throws on await_resume).
// ===========================================================================

static std::atomic<int> g_exc_chain{-1};

class ExceptionChainActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            try {
                auto thrower = []() -> qb::io::async::task<int> {
                    co_await qb::io::async::sleep(5ms);
                    throw std::runtime_error("inner error");
                    co_return 0;
                };
                [[maybe_unused]] int v = co_await thrower();
                ctx.template push<ValueEvent>(0); // should not reach
            } catch (const std::runtime_error &e) {
                EXPECT_STREQ(e.what(), "inner error");
                ctx.template push<ValueEvent>(1);
            }
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 1);
        g_exc_chain.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, ExceptionPropagationInChain) {
    g_exc_chain      = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<ExceptionChainActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_exc_chain.load(), 1) << "an exception in an awaited inner task must surface to the outer catch";
}

// ===========================================================================
// 10. Spawn from event handler: one coroutine spawned per inbound event.
// ===========================================================================

struct TriggerEvent : qb::Event {
    int value{0};
    TriggerEvent() = default;
    explicit TriggerEvent(int v)
        : value(v) {}
};

static std::atomic<int> g_perevent_sum{-1};

class SpawnPerEventActor : public qb::Actor {
    int                  results_sum_{0};
    int                  received_count_{0};
    static constexpr int EXPECTED = 4;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<TriggerEvent>(*this);
        registerEvent<ValueEvent>(*this);
        for (int i = 1; i <= EXPECTED; ++i)
            push<TriggerEvent>(id(), i);
        co_return true;
    }

    void
    on(const TriggerEvent &ev) {
        int val = ev.value;
        spawn_detached([val](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            ctx.template push<ValueEvent>(val * 10);
        });
    }

    void
    on(const ValueEvent &ev) {
        results_sum_ += ev.value;
        if (++received_count_ >= EXPECTED) {
            EXPECT_EQ(results_sum_, 100); // 10+20+30+40
            g_perevent_sum.store(results_sum_, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

TEST(ActorCoroutineAdvanced, SpawnCoroutinePerEvent) {
    g_perevent_sum   = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<SpawnPerEventActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_perevent_sum.load(), 100) << "one coroutine per event, results summed exactly";
}

// ===========================================================================
// 11. Coroutine with zero delay — immediate-completion path.
// ===========================================================================

static std::atomic<int> g_zero_delay{-1};

class ZeroDelayActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(0ms);
            ctx.template push<ValueEvent>(42);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 42);
        g_zero_delay.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, ZeroDelayCoroutine) {
    g_zero_delay     = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<ZeroDelayActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_zero_delay.load(), 42) << "a sleep(0ms) coroutine must still deliver";
}

// ===========================================================================
// 12. CoroContext::id() matches the spawning actor.
// ===========================================================================

static std::atomic<bool> g_id_match{false};

class IdCheckActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        ActorId self = id();
        spawn_detached([self](auto ctx) -> qb::io::async::task<void> {
            const bool before = (ctx.id() == self);
            co_await qb::io::async::sleep(5ms);
            const bool after = (ctx.id() == self);
            g_id_match.store(before && after, std::memory_order_relaxed);
            ctx.template push<DoneEvent>();
        });
        co_return true;
    }

    void
    on(const DoneEvent &) {
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, CoroContextIdMatchesActor) {
    g_id_match       = false;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<IdCheckActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_TRUE(g_id_match.load()) << "ctx.id() must equal the spawning actor's id, before and after suspension";
}

// ===========================================================================
// 13. Multiple actors each spawning a coroutine on the same core — sum is exact.
// ===========================================================================

static std::atomic<int> g_multi_actor_sum{0};
static std::atomic<int> g_multi_actor_done{0};

class MultiActorCoroWorker : public qb::Actor {
    int worker_id_;
    int total_workers_;

public:
    MultiActorCoroWorker(int id, int total)
        : worker_id_(id)
        , total_workers_(total) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        int wid = worker_id_;
        spawn_detached([wid](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(std::chrono::milliseconds(5 + wid));
            g_multi_actor_sum.fetch_add(wid, std::memory_order_relaxed);
            ctx.template push<DoneEvent>();
        });
        co_return true;
    }

    void
    on(const DoneEvent &) {
        if (g_multi_actor_done.fetch_add(1, std::memory_order_relaxed) + 1 >= total_workers_)
            qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, MultipleActorsWithCoroutinesOnSameCore) {
    g_multi_actor_sum  = 0;
    g_multi_actor_done = 0;
    g_watchdog_fired   = false;
    qb::Main      main;
    constexpr int N = 10;
    for (int i = 1; i <= N; ++i)
        main.addActor<MultiActorCoroWorker>(0, i, N);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_multi_actor_sum.load(), 55) << "1+2+...+10";
}

// ===========================================================================
// 14. Loop-variable capture safety (classic coroutine pitfall) — each coro gets
//     its own copy of i, so the sum is exact.
// ===========================================================================

static std::atomic<int> g_loop_sum{-1};

class LoopCaptureActor : public qb::Actor {
    int                  sum_{0};
    int                  count_{0};
    static constexpr int N = 5;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        for (int i = 0; i < N; ++i) {
            spawn_detached([i](auto ctx) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(5ms);
                ctx.template push<ValueEvent>(i);
            });
        }
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        sum_ += ev.value;
        if (++count_ >= N) {
            EXPECT_EQ(sum_, 10); // 0+1+2+3+4
            g_loop_sum.store(sum_, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

TEST(ActorCoroutineAdvanced, LoopVariableCapturedByValue) {
    g_loop_sum       = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<LoopCaptureActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_loop_sum.load(), 10) << "each coroutine must capture its own loop value";
}

// ===========================================================================
// 15. Nested spawn_detached inside the first coroutine (spawn from coro body).
// ===========================================================================

static std::atomic<int> g_nested_stage{-1};

class NestedSpawnActor : public qb::Actor {
    int stage_{0};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([this](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            spawn_detached([](auto ctx2) -> qb::io::async::task<void> {
                co_await qb::io::async::sleep(5ms);
                ctx2.template push<ValueEvent>(2);
            });
            ctx.template push<ValueEvent>(1);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        stage_ += ev.value;
        if (stage_ >= 3) {
            EXPECT_EQ(stage_, 3); // 1 + 2
            g_nested_stage.store(stage_, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

TEST(ActorCoroutineAdvanced, NestedSpawnFromCoroutineBody) {
    g_nested_stage   = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<NestedSpawnActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_nested_stage.load(), 3) << "a coroutine spawned from a coroutine body must also deliver";
}

// ===========================================================================
// 16. Coroutine outlives its actor — shared_ptr RAII counter must not crash.
//     DE-FLAKED: the keeper no longer races a 200ms timer against the 30ms
//     orphan; it stops the engine ONLY when the orphan signals completion, so
//     ordering is event-driven, not wall-clock.
// ===========================================================================

static std::atomic<bool> g_orphan_coro_completed{false};

struct OrphanDoneEvent : qb::Event {};

class OrphanCoroActor : public qb::Actor {
    ActorId keeper_;

public:
    explicit OrphanCoroActor(ActorId keeper)
        : keeper_(keeper) {}

    qb::io::async::task<bool>
    onInit() override {
        ActorId keeper = keeper_;
        spawn_detached([keeper](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(30ms);
            // The spawning actor is long dead by now; the detached coroutine still runs and the
            // RAII guard decrements via a shared_ptr (no dangling pointer to the dead actor).
            g_orphan_coro_completed.store(true, std::memory_order_relaxed);
            ctx.template push_to<OrphanDoneEvent>(keeper); // wakes the keeper deterministically
        });
        EXPECT_TRUE(has_active_coroutines());
        kill(); // die immediately, while the coroutine is suspended on the 30ms timer
        co_return true;
    }
};

class KeeperActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OrphanDoneEvent>(*this);
        co_return true; // stays alive until the orphan signals — keeps the engine running
    }

    void
    on(const OrphanDoneEvent &) {
        qb::Main::stop(); // event-driven shutdown: the orphan finished
    }
};

TEST(ActorCoroutineAdvanced, CoroutineOutlivesActor) {
    g_orphan_coro_completed = false;
    g_watchdog_fired        = false;
    qb::Main main;
    auto     keeper = main.addActor<KeeperActor>(0);
    ASSERT_NE(keeper, ActorId::NotFound);
    main.addActor<OrphanCoroActor>(0, keeper);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_TRUE(g_orphan_coro_completed.load()) << "a detached coroutine must complete even after its spawning actor died";
}

// ===========================================================================
// 17. Multi-core: each VirtualCore runs its own scheduler independently.
// ===========================================================================

static std::atomic<int> g_multicore_sum{0};
static std::atomic<int> g_multicore_done{0};

class MultiCoreCoroActor : public qb::Actor {
    int value_;

public:
    explicit MultiCoreCoroActor(int v)
        : value_(v) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        int val = value_;
        spawn_detached([val](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(10ms);
            g_multicore_sum.fetch_add(val, std::memory_order_relaxed);
            ctx.template push<DoneEvent>();
        });
        co_return true;
    }

    void
    on(const DoneEvent &) {
        if (g_multicore_done.fetch_add(1, std::memory_order_relaxed) + 1 >= 2)
            qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, MultiCoreCoroutines) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to run a scheduler per VirtualCore";
    }
    g_multicore_sum  = 0;
    g_multicore_done = 0;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<MultiCoreCoroActor>(0, 10);
    main.addActor<MultiCoreCoroActor>(1, 20);
    main.addActor<WatchdogActor>(0, 5s);
    main.addActor<WatchdogActor>(1, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_multicore_sum.load(), 30) << "each core's coroutine contributes its value (10 + 20)";
}

// ===========================================================================
// 18. Move-only capture: a unique_ptr survives suspension.
//     (Pruned the in-coroutine *p constant checks; the handler value is the oracle.)
// ===========================================================================

static std::atomic<int> g_moveonly_value{-1};

class MoveOnlyCaptureActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        auto ptr = std::make_unique<int>(42);
        spawn_detached([p = std::move(ptr)](auto ctx) mutable -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            int val = *p; // dereference after suspension: move-only state survived
            ctx.template push<ValueEvent>(val);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 42);
        g_moveonly_value.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, MoveOnlyCaptureUniquePtrSurvives) {
    g_moveonly_value = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<MoveOnlyCaptureActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_moveonly_value.load(), 42) << "a move-only (unique_ptr) capture must survive suspension";
}

// ===========================================================================
// 19. Minimal coroutine: no suspension, immediate co_return.
// ===========================================================================

static std::atomic<bool> g_immediate_coro_ran{false};

class ImmediateCoroActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<DoneEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            g_immediate_coro_ran.store(true, std::memory_order_relaxed);
            ctx.template push<DoneEvent>();
            co_return;
        });
        co_return true;
    }

    void
    on(const DoneEvent &) {
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, MinimalCoroutineNoSuspension) {
    g_immediate_coro_ran = false;
    g_watchdog_fired     = false;
    qb::Main main;
    main.addActor<ImmediateCoroActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_TRUE(g_immediate_coro_ran.load()) << "a no-suspension coroutine must run and deliver";
}

// ===========================================================================
// 20. Many sequential co_await in a single coroutine (20 suspensions) — every
//     suspension resumes, so both the per-step count and the final value match.
// ===========================================================================

static std::atomic<int> g_deep_suspend_count{0};
static std::atomic<int> g_deep_final{-1};

class DeepSuspensionActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        static constexpr int DEPTH = 20;
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            int accumulator = 0;
            for (int i = 0; i < DEPTH; ++i) {
                co_await qb::io::async::sleep(1ms);
                ++accumulator;
                g_deep_suspend_count.fetch_add(1, std::memory_order_relaxed);
            }
            ctx.template push<ValueEvent>(accumulator);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 20);
        g_deep_final.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, DeepSequentialSuspensions) {
    g_deep_suspend_count = 0;
    g_deep_final         = -1;
    g_watchdog_fired     = false;
    qb::Main main;
    main.addActor<DeepSuspensionActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_deep_suspend_count.load(), 20) << "every one of the 20 suspensions must resume";
    EXPECT_EQ(g_deep_final.load(), 20) << "the accumulator after 20 resumptions";
}

// ===========================================================================
// 21. when_all combinator inside spawn_detached.
// ===========================================================================

static std::atomic<int> g_whenall_sum{-1};

class WhenAllActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            auto work = [](int x) -> qb::io::async::task<int> {
                co_await qb::io::async::sleep(5ms);
                co_return x * 2;
            };
            auto [a, b, c] = co_await qb::io::async::when_all(work(1), work(2), work(3));
            ctx.template push<ValueEvent>(a + b + c); // 2+4+6 = 12
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 12);
        g_whenall_sum.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, WhenAllCombinatorInsideSpawnAsync) {
    g_whenall_sum    = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<WhenAllActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_whenall_sum.load(), 12) << "when_all must gather all three results (2+4+6)";
}

// ===========================================================================
// 22. coro_with_timeout: the inner task completes before the deadline.
// ===========================================================================

static std::atomic<int> g_timeout_success{-1};

class TimeoutSuccessActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            auto fast_work = []() -> qb::io::async::task<int> {
                co_await qb::io::async::sleep(5ms);
                co_return 99;
            };
            int result = co_await qb::io::async::coro_with_timeout(fast_work(), 200ms);
            ctx.template push<ValueEvent>(result);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 99);
        g_timeout_success.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, TimeoutCompletesBeforeDeadline) {
    g_timeout_success = -1;
    g_watchdog_fired  = false;
    qb::Main main;
    main.addActor<TimeoutSuccessActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_timeout_success.load(), 99) << "a task that finishes before the deadline must return its value";
}

// ===========================================================================
// 23. coro_with_timeout: the inner task exceeds the deadline → timeout_error.
// ===========================================================================

static std::atomic<int> g_timeout_exceeded{-1};

class TimeoutExceededActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        spawn_detached([](auto ctx) -> qb::io::async::task<void> {
            auto slow_work = []() -> qb::io::async::task<int> {
                co_await qb::io::async::sleep(500ms);
                co_return 0;
            };
            try {
                co_await qb::io::async::coro_with_timeout(slow_work(), 10ms);
                ctx.template push<ValueEvent>(0); // should not reach
            } catch (const qb::io::async::timeout_error &) {
                ctx.template push<ValueEvent>(1); // timeout caught
            }
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 1);
        g_timeout_exceeded.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, TimeoutExceedsDeadline) {
    g_timeout_exceeded = -1;
    g_watchdog_fired   = false;
    qb::Main main;
    main.addActor<TimeoutExceededActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_timeout_exceeded.load(), 1) << "a task exceeding its deadline must raise timeout_error";
}

// ===========================================================================
// 24. Cross-core push_to via CoroContext (requires-multicore).
// ===========================================================================

static std::atomic<int> g_cross_core_value{-1};

class CrossCoreReceiverActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 1234);
        g_cross_core_value.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

class CrossCoreSenderActor : public qb::Actor {
    ActorId target_;

public:
    explicit CrossCoreSenderActor(ActorId t)
        : target_(t) {}

    qb::io::async::task<bool>
    onInit() override {
        ActorId dest = target_;
        spawn_detached([dest](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(10ms);
            ctx.template push_to<ValueEvent>(dest, 1234);
        });
        co_return true;
    }
};

TEST(ActorCoroutineAdvanced, CrossCorePushTo) {
    if (std::thread::hardware_concurrency() < 2) {
        GTEST_SKIP() << "requires-multicore: needs >= 2 cores to place sender and receiver on distinct cores";
    }
    g_cross_core_value = -1;
    g_watchdog_fired   = false;
    qb::Main main;
    auto     receiver = main.addActor<CrossCoreReceiverActor>(1); // core 1
    ASSERT_NE(receiver, ActorId::NotFound);
    main.addActor<CrossCoreSenderActor>(0, receiver); // core 0
    main.addActor<WatchdogActor>(0, 5s);
    main.addActor<WatchdogActor>(1, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_cross_core_value.load(), 1234) << "ctx.push_to must deliver across cores";
}

// ===========================================================================
// 25. Concurrent spawns from multiple event handlers (not just onInit).
// ===========================================================================

struct WaveEvent : qb::Event {
    int wave{0};
    WaveEvent() = default;
    explicit WaveEvent(int w)
        : wave(w) {}
};

static std::atomic<int> g_wave_total{-1};

class MultiHandlerSpawnActor : public qb::Actor {
    int                  total_{0};
    int                  remaining_{0};
    static constexpr int WAVES = 3;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WaveEvent>(*this);
        registerEvent<ValueEvent>(*this);
        for (int w = 1; w <= WAVES; ++w)
            push<WaveEvent>(id(), w);
        remaining_ = WAVES;
        co_return true;
    }

    void
    on(const WaveEvent &ev) {
        int w = ev.wave;
        spawn_detached([w](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(std::chrono::milliseconds(5 * w));
            ctx.template push<ValueEvent>(w * 100);
        });
    }

    void
    on(const ValueEvent &ev) {
        total_ += ev.value;
        if (--remaining_ <= 0) {
            EXPECT_EQ(total_, 600); // 100+200+300
            g_wave_total.store(total_, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

TEST(ActorCoroutineAdvanced, SpawnFromMultipleEventHandlers) {
    g_wave_total     = -1;
    g_watchdog_fired = false;
    qb::Main main;
    main.addActor<MultiHandlerSpawnActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_EQ(g_wave_total.load(), 600) << "a coroutine per handler invocation, results summed exactly";
}

// ===========================================================================
// 26. Multiple captures of different types — the ONE kept capture-safety witness.
//     This single test retains in-coroutine capture checks (as a deliberate
//     witness that the compiler builds the coroutine frame correctly across
//     several heterogeneous captures); every other capture test relies on the
//     post-push handler value plus ASan for frame-capture UB.
// ===========================================================================

static std::atomic<int>  g_mixed_value{-1};
static std::atomic<bool> g_mixed_captures_ok{false};

class MixedCaptureActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ValueEvent>(*this);
        int                 num     = 7;
        std::string         name    = "hello";
        std::vector<double> weights = {1.5, 2.5, 3.5};
        ActorId             self    = id();

        spawn_detached([num, name, weights, self](auto ctx) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(5ms);
            const bool ok = num == 7 && name == "hello" && weights.size() == 3u && weights[0] == 1.5 && weights[1] == 2.5 && weights[2] == 3.5
                            && self == ctx.id();
            g_mixed_captures_ok.store(ok, std::memory_order_relaxed);
            co_await qb::io::async::sleep(5ms);
            int result = num + static_cast<int>(weights.size());
            ctx.template push<ValueEvent>(result);
        });
        co_return true;
    }

    void
    on(const ValueEvent &ev) {
        EXPECT_EQ(ev.value, 10); // 7 + 3
        g_mixed_value.store(ev.value, std::memory_order_relaxed);
        qb::Main::stop();
    }
};

TEST(ActorCoroutineAdvanced, MixedTypeCapturesSurvive) {
    g_mixed_value       = -1;
    g_mixed_captures_ok = false;
    g_watchdog_fired    = false;
    qb::Main main;
    main.addActor<MixedCaptureActor>(0);
    main.addActor<WatchdogActor>(0, 5s);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_NO_WATCHDOG();
    EXPECT_TRUE(g_mixed_captures_ok.load()) << "all heterogeneous captures must survive suspension";
    EXPECT_EQ(g_mixed_value.load(), 10) << "the derived value must round-trip through the handler";
}
