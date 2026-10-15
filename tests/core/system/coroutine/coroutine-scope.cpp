/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-scope.cpp
 * @brief Actor-scoped coroutines: `spawn` + `ScopedCoroContext` + cooperative cancel-on-kill.
 *
 * This is the canonical scope/cancellation suite for the `coroutine/` group. Unlike `spawn_detached`
 * (coroutine-detached.cpp), a coroutine launched via `Actor::spawn` is bound to a per-actor
 * cancellation scope: when the actor is killed/destroyed, every parked `ScopedCoroContext` operation
 * is *woken* and unwound with `cancelled_error` — it does NOT block for the remainder of a long
 * sleep. Validates the contract from internal/plans/QB_ACTOR_CORO_SCOPE.md across each awaiter:
 *   - `ctx.sleep`, `ctx.until_cancelled()`, `ctx.cancellation_point()`, `ctx.cancellable(task)`,
 *     `ctx.child_token()` + `qb::io::async::cancellable_sleep` are all cut short on kill;
 *   - the normal (non-killed) path completes and `ctx.push` works;
 *   - `spawn_detached` stays detached ("orphan-and-complete") — side-by-side with a scoped coro;
 *   - lazy scope allocation (`has_coro_scope()` is false until the first `spawn`);
 *   - `active_coroutine_count()` counts scoped coroutines;
 *   - NO frame leak: `qb::io::async::detail::CoroutineFrameAllocator::live_frames` returns to the
 *     pre-run baseline after the spawn → park → kill → reclaim cycle (the real reclamation oracle).
 *
 * Hardening over the original (see dev/tests-audit/qb-core/qbcore-c09.md):
 *   - the composite-bool lazy-allocation test is SPLIT into three independent assertions so a
 *     failure localises (before / after-detached / after-scoped);
 *   - the load-sensitive cases are made event-driven where possible: the multi-step cancel proves
 *     *where* cancellation landed via a step counter (kill margin widened to a 1s mid-step await so
 *     a slow CI tick cannot land outside the window), and the cancellation-point loop self-signals
 *     once it has made progress, so the kill is triggered by an observed iteration rather than a
 *     bare wall-clock window;
 *   - the `live_frames == baseline` leak check is extended to the `cancellable` and
 *     `child_token`/`cancellable_sleep` paths (which arm a detached timer — the more interesting
 *     reclamation case), not only the no-detached-timer `until_cancelled` path.
 *
 * Every in-coroutine effect is mirrored to a file-scope atomic reset before the run and asserted
 * AFTER `join()` (no pass-if-never-run).
 *
 * The thirteen `qb::io::async::callback([this] { if (is_alive()) kill(); }, N)` timers below are
 * DELIBERATE and must not be swept into `spawn` + `co_await ctx.sleep` (audited 2026-08; all
 * thirteen kept). Two independent reasons:
 *   - They are SAFE. The general hazard — a timed callback is owned by the event loop and outlives
 *     its actor, so the `is_alive()` guard IS the read of freed memory — requires the actor to die
 *     from some OTHER cause while the timer is still pending. Here this callback is each actor's
 *     ONLY death: the core loop breaks only on `_actors.empty()`, which nothing else can bring
 *     about, so neither the reap nor `listener::clear()` (which reclaims a pending `Timeout`
 *     WITHOUT firing it) can run first. Measured: zero ASan reports over this file.
 *   - The trigger is deliberately OUT-OF-BAND. It must not be a participant in the actor
 *     cancellation scope that is this file's subject; converting puts the instrument inside the
 *     thing being instrumented — a second scoped coroutine on the actor under test, inside the same
 *     `active_coroutines_` counter and the same `CoroutineFrameAllocator` accounting that the four
 *     `live_frames == baseline` tests use as their reclamation oracle. Note that this perturbation
 *     would be SILENT rather than caught: both `active_coroutine_count()` reads below happen just
 *     BEFORE the timer is armed, so neither would observe the extra coroutine.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Shared observation flags (reset at the start of each TEST).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_scoped_completed_full{false};  // set if a long ctx.sleep ran to the end
std::atomic<bool> g_scoped_cancel_observed{false}; // set in the coroutine's catch on cancel
std::atomic<bool> g_raw_completed{false};          // set if a raw spawn_detached coroutine finished
std::atomic<int>  g_loop_iterations{0};            // cancellation_point loop progress

void
reset_flags() {
    g_scoped_completed_full  = false;
    g_scoped_cancel_observed = false;
    g_raw_completed          = false;
    g_loop_iterations        = 0;
}
} // namespace

struct ScopeResultEvent : public qb::Event {
    int value{0};
    ScopeResultEvent() = default;
    explicit ScopeResultEvent(int v)
        : value(v) {}
};

// ---------------------------------------------------------------------------
// 1. Cancel-on-kill wakes a scoped coroutine parked on a long ctx.sleep.
// ---------------------------------------------------------------------------
class ScopedKillActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Parked on a 2s cancellable sleep — must be cut short by kill, not block.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.sleep(2s);
                g_scoped_completed_full = true; // MUST NOT run
            } catch (const qb::io::async::cancelled_error &) {
                g_scoped_cancel_observed = true; // SHOULD run
            }
        });
        // Kill shortly after, while the coroutine is parked on the 2s sleep.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, CancelOnKillWakesScopedCoroutine) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedKillActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_scoped_completed_full.load()) << "the 2s sleep should have been cancelled";
    EXPECT_TRUE(g_scoped_cancel_observed.load()) << "cancellation must be observed on kill";
}

// ---------------------------------------------------------------------------
// 2. Happy path: a scoped coroutine completes normally and ctx.push works.
// ---------------------------------------------------------------------------
class ScopedNormalActor : public qb::Actor {
    bool got_{false};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ScopeResultEvent>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(10ms);        // completes normally (not cancelled)
            ctx.push<ScopeResultEvent>(123); // safe push back to self
        });
        co_return true;
    }

    void
    on(const ScopeResultEvent &ev) {
        EXPECT_EQ(ev.value, 123);
        got_ = true;
        // The coroutine has completed; no scoped coroutine should remain.
        EXPECT_EQ(active_coroutine_count(), 0u);
        kill();
    }

    ~ScopedNormalActor() override {
        EXPECT_TRUE(got_);
    }
};

TEST(ActorCoroutineScope, ScopedCoroutineNormalCompletion) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedNormalActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 3. Differentiator: on kill, a scoped coroutine is cancelled while a raw
//    spawn_detached coroutine remains detached and completes ("orphan-and-complete").
//    A keeper actor keeps the engine alive long enough to observe both.
// ---------------------------------------------------------------------------
class ScopedVsAsyncActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Scoped: cancelled at kill (its 40ms sleep must NOT finish).
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(40ms);
            g_scoped_completed_full = true; // MUST NOT run (cancelled at ~10ms)
        });
        // Raw: detached — survives the actor and completes its 40ms sleep.
        spawn_detached([](auto) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(40ms);
            g_raw_completed = true; // SHOULD run (orphan-and-complete)
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            10ms);
        co_return true;
    }
};

class KeeperActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        // Keep the engine alive past the 40ms coroutine deadlines, then stop.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            150ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, ScopedCancelledButAsyncOrphanCompletes) {
    reset_flags();
    qb::Main main;
    main.addActor<ScopedVsAsyncActor>(0);
    main.addActor<KeeperActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_scoped_completed_full.load()) << "scoped coroutine must be cancelled on kill";
    EXPECT_TRUE(g_raw_completed.load()) << "spawn_detached must stay detached (orphan-and-complete)";
}

// ---------------------------------------------------------------------------
// 4. Stress: many scoped coroutines cancelled at once — no crash, no frame leak.
// ---------------------------------------------------------------------------
class ManyScopedActor : public qb::Actor {
public:
    static constexpr int N = 32;

    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < N; ++i) {
            // until_cancelled() parks with NO detached timer/helper, so reclamation on
            // kill is exact — every coroutine frame is reaped (validates the leak check).
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                co_await ctx.until_cancelled(); // parks until the actor is killed
                g_scoped_completed_full = true; // MUST NOT run
            });
        }
        EXPECT_EQ(active_coroutine_count(), static_cast<std::size_t>(N));
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, ManyScopedCoroutinesCancelledNoLeak) {
    reset_flags();
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main main;
        main.addActor<ManyScopedActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_FALSE(g_scoped_completed_full.load());
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "all scoped coroutine frames must be reclaimed (no leak)";
}

// ---------------------------------------------------------------------------
// 5. ctx.cancellation_point() bails cooperatively on kill.
//    The kill is armed on the loop's OBSERVED progress, not after a delay believed to
//    outlast it. What this replaced was 60ms against a 2ms loop period, documented as
//    "~30 iterations ... cannot flake under load" -- arithmetic that holds only where a
//    2ms sleep costs 2ms. Windows' default timer granularity is ~15.6ms, so 60ms buys
//    three or four iterations rather than thirty, and one missed tick empties the
//    margin: measured here, one failure in two full `debug` suite runs, g_loop_iterations
//    at 0. Waiting on the counter makes "made progress before cancel" true by
//    CONSTRUCTION, on any platform and under any load.
// ---------------------------------------------------------------------------
class CancellationPointActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                for (;;) {
                    co_await ctx.cancellation_point(); // the sole operation that can throw cancellation
                    co_await qb::io::async::sleep(2ms);
                    g_loop_iterations.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const qb::io::async::cancelled_error &) {
                g_scoped_cancel_observed = true;
            }
        });
        arm_kill();
        co_return true;
    }

private:
    // Three rather than one: the property under test is a loop cancelled MID-FLIGHT, and a
    // single iteration is a weaker statement than that. Three costs ~6ms where a sleep is
    // honoured and ~47ms where it is rounded up to the timer tick -- bounded either way,
    // unlike a wall-clock deadline. The attempt cap turns a loop that never progresses into
    // a failed assertion rather than a hung suite.
    static constexpr int MIN_ITERATIONS = 3;

    void
    arm_kill(int attempts_left = 100) {
        qb::io::async::callback(
            [this, attempts_left] {
                if (!is_alive())
                    return;
                if (g_loop_iterations.load() < MIN_ITERATIONS && attempts_left > 0) {
                    arm_kill(attempts_left - 1); // not far enough in yet -- look again
                    return;
                }
                kill(); // provably inside a loop that has made progress
            },
            5ms);
    }
};

// Keep the core alive until the uncancellable pacing sleep reaches the explicit
// cancellation point, or until the test's failure deadline expires.
class CancellationPointKeeper
    : public qb::Actor
    , public qb::ICallback {
    qb::mono_time _deadline{};

public:
    qb::io::async::task<bool>
    onInit() override {
        _deadline = qb::mono_now() + 2s;
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        if (g_scoped_cancel_observed.load() || qb::mono_now() >= _deadline)
            kill();
    }
};

TEST(ActorCoroutineScope, CancellationPointBailsOnKill) {
    reset_flags();
    qb::Main main;
    main.addActor<CancellationPointActor>(0);
    main.addActor<CancellationPointKeeper>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_scoped_cancel_observed.load()) << "cancellation_point must throw on kill";
    EXPECT_GT(g_loop_iterations.load(), 0) << "the loop must have made progress (≈30 iterations) before cancel";
}

// ---------------------------------------------------------------------------
// 6. ctx.cancellable(task) — an arbitrary task wrapped by the scope is cancelled.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_cancellable_done{false};
std::atomic<bool> g_cancellable_cancelled{false};

qb::io::async::task<int>
scope_slow_task() {
    co_await qb::io::async::sleep(2s);
    co_return 7;
}
} // namespace

class CancellableActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.cancellable(scope_slow_task()); // cancelled on kill
                g_cancellable_done = true;                   // MUST NOT run
            } catch (const qb::io::async::cancelled_error &) {
                g_cancellable_cancelled = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, CancellableWrapperCancelledOnKill) {
    g_cancellable_done      = false;
    g_cancellable_cancelled = false;
    qb::Main main;
    main.addActor<CancellableActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_FALSE(g_cancellable_done.load()) << "the 2s wrapped task must be cancelled, not completed";
    EXPECT_TRUE(g_cancellable_cancelled.load()) << "ctx.cancellable() must surface cancelled_error on kill";
}

// ---------------------------------------------------------------------------
// 7. child_token() is cancelled when the actor scope is cancelled.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_child_cancelled{false};
} // namespace

class ChildTokenActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto child = ctx.child_token(); // linked to the actor scope
            try {
                co_await qb::io::async::cancellable_sleep(2s, child);
            } catch (const qb::io::async::cancelled_error &) {
                g_child_cancelled = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, ChildTokenCancelledWithParentScope) {
    g_child_cancelled = false;
    qb::Main main;
    main.addActor<ChildTokenActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_child_cancelled.load()) << "a child_token must be cancelled when its parent scope is";
}

// ---------------------------------------------------------------------------
// 8. Lazy scope: has_coro_scope() is false until the first spawn; an
//    actor that only uses spawn_detached (never allocates a scope) kills cleanly.
// ---------------------------------------------------------------------------
namespace {
// Three independent observations (split from the old single composite bool so a failure localises).
// -1 == not yet observed; 0 == false; 1 == true.
std::atomic<int> g_scope_before{-1};       // has_coro_scope() before any spawn — expect false (0)
std::atomic<int> g_scope_after_async{-1};  // after spawn_detached — STILL false (0): detached ≠ scope
std::atomic<int> g_scope_after_scoped{-1}; // after spawn — true (1): the scope is lazily allocated
} // namespace

class ScopeFlagActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_scope_before.store(has_coro_scope() ? 1 : 0, std::memory_order_relaxed); // false
        spawn_detached([](auto) -> qb::io::async::task<void> { co_await qb::io::async::sleep(5ms); });
        g_scope_after_async.store(has_coro_scope() ? 1 : 0, std::memory_order_relaxed); // still false
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.sleep(5ms); });
        g_scope_after_scoped.store(has_coro_scope() ? 1 : 0, std::memory_order_relaxed); // true
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, HasCoroScopeReflectsLazyAllocation) {
    g_scope_before       = -1;
    g_scope_after_async  = -1;
    g_scope_after_scoped = -1;
    qb::Main main;
    main.addActor<ScopeFlagActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    // Each observation asserted independently: a failure tells you exactly which invariant broke.
    EXPECT_EQ(g_scope_before.load(), 0) << "has_coro_scope() must be false before any spawn";
    EXPECT_EQ(g_scope_after_async.load(), 0) << "spawn_detached must NOT allocate a scope (detached ≠ scoped)";
    EXPECT_EQ(g_scope_after_scoped.load(), 1) << "spawn must lazily allocate the cancellation scope";
}

// ---------------------------------------------------------------------------
// 9. A scoped coroutine with several sequential awaits is cancelled mid-sequence.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int>  g_steps{0};
std::atomic<bool> g_seq_cancelled{false};
} // namespace

class MultiStepActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            // Step 1 is a tiny (10ms) await that completes; step 2 is a deliberately LONG (1s)
            // await, and the kill is armed to land strictly inside it. The step counter then
            // proves cancellation landed in step 2 rather than anywhere else.
            try {
                co_await ctx.sleep(10ms);
                g_steps.fetch_add(1); // step 1 — completes before kill
                co_await ctx.sleep(1s);
                g_steps.fetch_add(1); // killed during this await — never reached
                co_await ctx.sleep(10ms);
                g_steps.fetch_add(1);
            } catch (const qb::io::async::cancelled_error &) {
                g_seq_cancelled = true;
            }
        });
        arm_kill();
        co_return true;
    }

private:
    // Arm the kill on step 1's OBSERVED completion, not on a delay chosen to be longer than it.
    //
    // A fixed 50ms callback raced step 1's 10ms sleep: the two timers are independent, so on a
    // loaded machine the 50ms one can be serviced before the 10ms one and the kill then cancels
    // step 1, leaving g_steps at 0. Measured on Windows/MSVC 19.51 — 20/20 passes run alone, 6
    // failures in 43 runs under 8-way concurrency, every failure with g_steps == 0. `ctest` runs
    // these presets at jobs:4, so "alone" is not the condition the suite actually provides.
    //
    // Re-arming until the counter moves makes step 2 the cancellation point by CONSTRUCTION, on
    // any machine and under any load — the property the test means to assert. Bounded, so a step 1
    // that genuinely never completes fails the assertion loudly instead of hanging the suite.
    void
    arm_kill(int attempts_left = 100) {
        qb::io::async::callback(
            [this, attempts_left] {
                if (!is_alive())
                    return;
                if (g_steps.load() == 0 && attempts_left > 0) {
                    arm_kill(attempts_left - 1); // step 1 has not landed yet — look again
                    return;
                }
                kill(); // provably inside step 2's 1s await
            },
            5ms);
    }
};

TEST(ActorCoroutineScope, MultiStepCancelledMidSequence) {
    g_steps         = 0;
    g_seq_cancelled = false;
    qb::Main main;
    main.addActor<MultiStepActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_steps.load(), 1) << "only the first (10ms) await completed before the kill landed mid-step-2";
    EXPECT_TRUE(g_seq_cancelled.load()) << "the long step-2 await must be cancelled, not run to its 1s deadline";
}

// ---------------------------------------------------------------------------
// 10. active_coroutine_count() counts scoped coroutines.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_count_at_spawn{-1};
} // namespace

class CountActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < 4; ++i)
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.until_cancelled(); });
        g_count_at_spawn = static_cast<int>(active_coroutine_count());
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            20ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, ActiveCountCountsScopedCoroutines) {
    g_count_at_spawn = -1;
    qb::Main main;
    main.addActor<CountActor>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_count_at_spawn.load(), 4);
}

// ---------------------------------------------------------------------------
// 11. Detached-timer reclamation (the real reclamation oracle for the paths
//     that arm a detached helper). Unlike `until_cancelled()` (no helper),
//     `ctx.cancellable(task)` and `ctx.child_token()` + `cancellable_sleep`
//     spawn a DETACHED helper coroutine that parks on the full original
//     duration. When the scope is cancelled mid-flight, that helper frame must
//     be reclaimed too — not left parked until its (e.g. 2s) timer fires. Measure
//     the frame drop inside the kill callback: ~VirtualCore now recycles the
//     scheduler and would hide a cancellation leak in a post-join count.
//
//     The measurement is valid because start(false) runs core 0 on THIS thread,
//     so the test thread's `thread_local live_frames` IS the worker's counter.
//     Baseline is captured immediately before each run so the assertion is
//     self-relative (independent of any residue from earlier tests).
// ---------------------------------------------------------------------------
namespace {
long
live_frames_now() {
    return qb::io::async::detail::CoroutineFrameAllocator::live_frames;
}
std::atomic<long> g_frames_before_kill{-1};
std::atomic<long> g_frames_after_kill{-1};
std::atomic<bool> g_helper_started{false};
} // namespace

class CancellableLeakActor
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                g_helper_started.store(true, std::memory_order_release);
                co_await ctx.cancellable(scope_slow_task()); // arms a detached runner + inner 2s task
            } catch (const qb::io::async::cancelled_error &) {
            }
        });
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        // The loop drains run_ready() before dispatching this callback. Seeing the flag here
        // proves the wrapper reached its await_suspend and both helper frames were created.
        if (!g_helper_started.load(std::memory_order_acquire))
            return;
        g_frames_before_kill = live_frames_now();
        kill();
        g_frames_after_kill = live_frames_now();
    }
};

TEST(ActorCoroutineScope, CancellableDetachedTimerReclaimedNoLeak) {
    g_frames_before_kill = g_frames_after_kill = -1;
    g_helper_started                           = false;
    const long baseline                        = live_frames_now();
    {
        qb::Main main;
        main.addActor<CancellableLeakActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    const long before = g_frames_before_kill.load();
    const long after  = g_frames_after_kill.load();
    ASSERT_GE(before, baseline + 2) << "the detached runner and inner task must be armed before kill";
    EXPECT_EQ(before - after, 2) << "kill must reclaim both helper frames before core teardown can recycle them";
    EXPECT_EQ(live_frames_now(), baseline) << "no coroutine frame may survive the engine";
}

class ChildSleepLeakActor
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto child = ctx.child_token();
            try {
                g_helper_started.store(true, std::memory_order_release);
                co_await qb::io::async::cancellable_sleep(2s, child); // arms a detached timer_task
            } catch (const qb::io::async::cancelled_error &) {
            }
        });
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        // run_ready() finishes before the core's LoopEvent callback, so the timer_task has
        // been spawned and drained to its sleep before this can observe g_helper_started.
        if (!g_helper_started.load(std::memory_order_acquire))
            return;
        g_frames_before_kill = live_frames_now();
        kill();
        g_frames_after_kill = live_frames_now();
    }
};

TEST(ActorCoroutineScope, CancellableSleepDetachedTimerReclaimedNoLeak) {
    g_frames_before_kill = g_frames_after_kill = -1;
    g_helper_started                           = false;
    const long baseline                        = live_frames_now();
    {
        qb::Main main;
        main.addActor<ChildSleepLeakActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    const long before = g_frames_before_kill.load();
    const long after  = g_frames_after_kill.load();
    ASSERT_GE(before, baseline + 1) << "the detached timer must be armed before kill";
    EXPECT_EQ(before - after, 1) << "kill must reclaim the timer frame before core teardown can recycle it";
    EXPECT_EQ(live_frames_now(), baseline) << "no coroutine frame may survive the engine";
}

// ---------------------------------------------------------------------------
// 13. Same-tick race: the inner op's OWN watcher fires in the same libev
//     iteration as the kill. When the inner timer has fired (its frame is
//     already queued for resume) and the scope is THEN cancelled in that same
//     iteration, the cancel path destroys the inner frame — it must first
//     scrub it from the scheduler ready/in-flight sets (`forget()`), otherwise
//     run_ready() would later pop a freed handle (use-after-free).
//
//     The inner sleep equals the kill delay so both timers tend to land in one
//     iteration. The winner (completed vs cancelled) is genuinely racy, so we
//     assert ONLY the invariants that must hold either way: no leak and no
//     crash (this case is the primary value of running the suite under ASan).
// ---------------------------------------------------------------------------
namespace {
std::atomic<int> g_sametick_done{0};      // inner op completed (won the race)
std::atomic<int> g_sametick_cancelled{0}; // scope cancel won the race

qb::io::async::task<int>
sametick_task() {
    co_await qb::io::async::sleep(15ms);
    co_return 1;
}
} // namespace

class SameTickRaceActor : public qb::Actor {
public:
    static constexpr int N = 16;

    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < N; ++i) {
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                try {
                    co_await ctx.cancellable(sametick_task());
                    g_sametick_done.fetch_add(1, std::memory_order_relaxed);
                } catch (const qb::io::async::cancelled_error &) {
                    g_sametick_cancelled.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        // Same deadline as the inner sleep ⇒ inner watcher + kill tend to fire in
        // the same libev iteration, exercising the watcher-fired-then-cancelled window.
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            15ms);
        co_return true;
    }
};

TEST(ActorCoroutineScope, CancellableSameTickWatcherFiredThenCancelledNoLeakNoUAF) {
    g_sametick_done      = 0;
    g_sametick_cancelled = 0;
    const long baseline  = live_frames_now();
    {
        qb::Main main;
        main.addActor<SameTickRaceActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    // Each of the N coroutines resolved exactly once (completed XOR cancelled) and
    // every frame — winner or loser — was reclaimed. (UAF, if any, is caught by ASan.)
    EXPECT_EQ(g_sametick_done.load() + g_sametick_cancelled.load(), SameTickRaceActor::N)
        << "every cancellable op must resolve exactly once (completed or cancelled)";
    EXPECT_EQ(live_frames_now(), baseline) << "no frame may leak regardless of which side of the race wins";
}

// QB-267 public-path witnesses: this source is identical for the control
// child_token implementation and the private candidate. No internal factory.
namespace coroutine_scope_qb267_test {

struct PublicChildHistoryObservation {
    std::atomic<int>  markers_created{0};
    std::atomic<int>  markers_destroyed{0};
    std::atomic<int>  child_callbacks{0};
    std::atomic<int>  parent_callbacks{0};
    std::atomic<int>  callbacks_baseline{-1};
    std::atomic<int>  hooks_baseline{-1};
    std::atomic<int>  callbacks_before_kill{-1};
    std::atomic<int>  hooks_before_kill{-1};
    std::atomic<int>  created_before_kill{-1};
    std::atomic<int>  destroyed_before_kill{-1};
    std::atomic<int>  child_callbacks_before_kill{-1};
    std::atomic<int>  actor_destroyed{0};
    std::atomic<bool> parent_alive_before_kill{false};
    std::atomic<bool> snapshot_taken{false};
};

struct ChildCallbackMarker {
    const std::shared_ptr<PublicChildHistoryObservation> observation;
    explicit ChildCallbackMarker(std::shared_ptr<PublicChildHistoryObservation> o)
        : observation(std::move(o)) {
        observation->markers_created.fetch_add(1, std::memory_order_relaxed);
    }
    ~ChildCallbackMarker() {
        observation->markers_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
};

// Public state census held by a live ScopedCoroContext. Its bound detects a
// corrupt cycle without spinning; no private child representation is inspected.
int
live_scope_hooks(const qb::io::async::cancellation_token &scope) {
    const auto *state = scope.get_state();
    if (!state)
        return -1;
    const auto *head  = &state->hooks;
    int         count = 0;
    for (const auto *hook = head->next; hook != head; hook = hook->next) {
        if (!hook || ++count > 4098)
            return -1;
    }
    return count;
}

class PublicChildHistoryActor
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<PublicChildHistoryObservation> observation_;
    bool                                           cancel_children_;
    bool                                           ran_ = false;

public:
    static constexpr int ChildCount = 4096;

    PublicChildHistoryActor(std::shared_ptr<PublicChildHistoryObservation> o, bool cancel_children)
        : observation_(std::move(o))
        , cancel_children_(cancel_children) {}

    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        if (ran_)
            return;
        ran_                       = true;
        const auto observation     = observation_;
        const bool cancel_children = cancel_children_;
        auto       ctx             = context(); // real actor scope, kept alive throughout every census
        ctx.token().on_cancel([observation] { observation->parent_callbacks.fetch_add(1, std::memory_order_relaxed); });
        observation->callbacks_baseline = static_cast<int>(ctx.token().get_state()->callbacks.size());
        observation->hooks_baseline     = live_scope_hooks(ctx.token());
        for (int i = 0; i < ChildCount; ++i) {
            // Each Marker is owned solely by the child callback. No external
            // Marker handle survives this registration, and observation owns no token.
            auto child = context().child_token();
            child.on_cancel([marker = std::make_shared<ChildCallbackMarker>(observation)] {
                marker->observation->child_callbacks.fetch_add(1, std::memory_order_relaxed);
            });
            if (cancel_children) {
                auto copy       = child;
                auto moved_copy = std::move(copy);
                child.cancel();
                // Both copied/moved handles die before the loop's child handle.
                // This is public token ownership, not a private node assertion.
                (void) moved_copy;
            }
        }
        // Snapshot before kill; later cancellation/actor destruction must not
        // repair an earlier failed observation and make history look bounded.
        observation->callbacks_before_kill       = static_cast<int>(ctx.token().get_state()->callbacks.size());
        observation->hooks_before_kill           = live_scope_hooks(ctx.token());
        observation->created_before_kill         = observation->markers_created.load(std::memory_order_relaxed);
        observation->destroyed_before_kill       = observation->markers_destroyed.load(std::memory_order_relaxed);
        observation->child_callbacks_before_kill = observation->child_callbacks.load(std::memory_order_relaxed);
        observation->parent_alive_before_kill    = is_alive() && !ctx.cancelled();
        observation->snapshot_taken.store(true, std::memory_order_release);
        kill(); // synchronous handler, outside a coroutine body; no actor access after this
    }

    ~PublicChildHistoryActor() override {
        observation_->actor_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
};

void
expect_public_child_history_bounded(bool cancel_children) {
    auto observation = std::make_shared<PublicChildHistoryObservation>();
    {
        qb::Main main;
        main.addActor<PublicChildHistoryActor>(0, observation, cancel_children);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    ASSERT_TRUE(observation->snapshot_taken.load(std::memory_order_acquire));
    EXPECT_TRUE(observation->parent_alive_before_kill.load());
    EXPECT_EQ(observation->created_before_kill.load(), PublicChildHistoryActor::ChildCount);
    EXPECT_EQ(observation->destroyed_before_kill.load(), PublicChildHistoryActor::ChildCount)
        << "child callback payloads must die before the live parent scope is killed";
    EXPECT_EQ(observation->child_callbacks_before_kill.load(), cancel_children ? PublicChildHistoryActor::ChildCount : 0);
    ASSERT_GE(observation->callbacks_baseline.load(), 1); // ordinary sentinel callback is live
    ASSERT_GE(observation->hooks_baseline.load(), 0);
    ASSERT_GE(observation->callbacks_before_kill.load(), 0);
    ASSERT_GE(observation->hooks_before_kill.load(), 0);
    EXPECT_LE(observation->callbacks_before_kill.load(), observation->callbacks_baseline.load())
        << "the live parent callback set contains completed-child history";
    EXPECT_LE(observation->hooks_before_kill.load(), observation->hooks_baseline.load())
        << "the live parent hook set contains completed-child history";
    // Cleanup evidence is independent of the immutable snapshots above. It
    // deliberately accepts the base's delayed passive callback delivery on kill.
    EXPECT_EQ(observation->parent_callbacks.load(), 1);
    EXPECT_EQ(observation->markers_created.load(), PublicChildHistoryActor::ChildCount);
    EXPECT_EQ(observation->markers_destroyed.load(), PublicChildHistoryActor::ChildCount);
    EXPECT_EQ(observation->actor_destroyed.load(), 1);
    if (cancel_children)
        EXPECT_EQ(observation->child_callbacks.load(), PublicChildHistoryActor::ChildCount);
}

TEST(ActorCoroutineScope, QB267PublicPassiveChildrenReleasePayloadsBeforeParentKill) {
    expect_public_child_history_bounded(false);
}

TEST(ActorCoroutineScope, QB267PublicCancelledChildCopiesLeaveNoLiveParentHistory) {
    expect_public_child_history_bounded(true);
}

} // namespace coroutine_scope_qb267_test

namespace coroutine_scope_qb267_topology_test {
using qb::io::async::cancellation_token;
using qb::io::async::null_token;

enum class Scenario { live_copies, cancelled_copies, nested_drop, reentrant };
struct Observation {
    std::atomic<int>  created{0};
    std::atomic<int>  destroyed{0};
    std::atomic<int>  delivered{0};
    std::atomic<bool> completed{false};
};
struct Marker {
    std::shared_ptr<Observation> observation;
    explicit Marker(std::shared_ptr<Observation> value)
        : observation(std::move(value)) {
        ++observation->created;
    }
    ~Marker() {
        ++observation->destroyed;
    }
};

int
hooks(cancellation_token const &token) {
    return coroutine_scope_qb267_test::live_scope_hooks(token);
}
std::size_t
callbacks(cancellation_token const &token) {
    return token.get_state() ? token.get_state()->callbacks.size() : 0;
}

class TopologyActor
    : public qb::Actor
    , public qb::ICallback {
    std::shared_ptr<Observation> _observation;
    Scenario                     _scenario;

public:
    TopologyActor(std::shared_ptr<Observation> observation, Scenario scenario)
        : _observation(std::move(observation))
        , _scenario(scenario) {}
    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        // Every token, context and state view lives on this core. No await or callback
        // outlives these locals; the actual actor is retained throughout each public call.
        const auto                  observation = _observation;
        cancellation_token          parent;
        const qb::ScopedCoroContext parent_ctx(this, parent);
        int                         parent_deliveries = 0;
        parent.on_cancel([&parent_deliveries] { ++parent_deliveries; });
        const auto base_callbacks = callbacks(parent);
        const int  base_hooks     = hooks(parent);
        const auto links          = [&] {
            return static_cast<int>(callbacks(parent) - base_callbacks) + hooks(parent) - base_hooks;
        };
        if (_scenario == Scenario::live_copies || _scenario == Scenario::cancelled_copies) {
            auto child  = parent_ctx.child_token();
            auto copied = child;
            auto moved  = std::move(copied);
            EXPECT_FALSE(static_cast<bool>(copied));
            EXPECT_EQ(child.get_state(), moved.get_state());
            EXPECT_EQ(links(), 1);                  // either public callback or hook while the child is live
            EXPECT_EQ(child.get_state()->refs, 2u); // exactly the two public handles
            auto                        marker = std::make_shared<Marker>(observation);
            const std::weak_ptr<Marker> weak   = marker;
            child.on_cancel([marker] { ++marker->observation->delivered; });
            marker.reset();
            if (_scenario == Scenario::cancelled_copies) {
                child.cancel();
                EXPECT_TRUE(moved.is_cancelled());
                EXPECT_FALSE(parent.is_cancelled());
                EXPECT_TRUE(weak.expired());
                EXPECT_EQ(callbacks(child), 0u);
                EXPECT_EQ(hooks(child), 0);
                EXPECT_EQ(links(), 0); // copied cancelled handles remain alive here
                EXPECT_EQ(observation->delivered.load(), 1);
            }
            child = cancellation_token(null_token);
            if (_scenario == Scenario::live_copies) {
                EXPECT_FALSE(weak.expired());
                EXPECT_EQ(links(), 1);
            }
            moved = cancellation_token(null_token);
            EXPECT_TRUE(weak.expired());
            EXPECT_EQ(links(), 0);
            EXPECT_EQ(callbacks(parent), base_callbacks);
            EXPECT_EQ(hooks(parent), base_hooks);
            EXPECT_FALSE(parent.is_cancelled()); // parent retained until all snapshots are taken
        } else if (_scenario == Scenario::nested_drop) {
            cancellation_token    grandchild(null_token);
            std::weak_ptr<Marker> middle_marker;
            {
                auto middle   = parent_ctx.child_token();
                grandchild    = qb::ScopedCoroContext(this, middle).child_token();
                auto marker   = std::make_shared<Marker>(observation);
                middle_marker = marker;
                middle.on_cancel([marker] { ++marker->observation->delivered; });
                marker.reset();
                EXPECT_FALSE(middle_marker.expired());
                EXPECT_EQ(links(), 1);
            }
            EXPECT_TRUE(middle_marker.expired());
            EXPECT_EQ(links(), 0);
            parent.cancel();
            EXPECT_FALSE(grandchild.is_cancelled());
            EXPECT_EQ(callbacks(parent), 0u);
            EXPECT_EQ(hooks(parent), 0);
            int grandchild_deliveries = 0;
            grandchild.on_cancel([&grandchild_deliveries] { ++grandchild_deliveries; });
            grandchild.cancel();
            EXPECT_EQ(grandchild_deliveries, 1);
            EXPECT_EQ(observation->destroyed.load(), 1);
        } else {
            auto                              first = parent_ctx.child_token();
            std::optional<cancellation_token> sibling(parent_ctx.child_token());
            int                               first_deliveries     = 0;
            int                               sibling_deliveries   = 0;
            int                               immediate_deliveries = 0;
            bool                              fresh_cancelled      = false;
            sibling->on_cancel([&sibling_deliveries] { ++sibling_deliveries; });
            first.on_cancel([&] {
                ++first_deliveries;
                sibling.reset();
                auto fresh      = parent_ctx.child_token();
                fresh_cancelled = fresh.is_cancelled();
                fresh.on_cancel([&immediate_deliveries] { ++immediate_deliveries; });
                parent.cancel(); // reentrant cancellation of the still-held parent
            });
            parent.cancel();
            EXPECT_TRUE(first.is_cancelled());
            EXPECT_TRUE(fresh_cancelled);
            EXPECT_EQ(first_deliveries, 1);
            EXPECT_LE(sibling_deliveries, 1); // sibling order is unspecified
            EXPECT_EQ(immediate_deliveries, 1);
            EXPECT_EQ(callbacks(parent), 0u);
            EXPECT_EQ(hooks(parent), 0);
            EXPECT_EQ(callbacks(first), 0u);
            EXPECT_EQ(hooks(first), 0);
        }
        parent.cancel();
        EXPECT_EQ(parent_deliveries, 1);
        observation->completed.store(true);
        kill();
    }
};

void
check_topology(Scenario scenario) {
    const auto observation = std::make_shared<Observation>();
    qb::Main   main;
    main.addActor<TopologyActor>(0, observation, scenario);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(observation->completed.load());
    EXPECT_EQ(observation->created.load(), observation->destroyed.load());
}
TEST(ActorCoroutineScope, QB267PublicLiveCopiesMoveAndLastDropUnlinkBeforeParentCancel) {
    check_topology(Scenario::live_copies);
}
TEST(ActorCoroutineScope, QB267PublicCancelledCopiesReleaseMarkersWhileParentRemainsAlive) {
    check_topology(Scenario::cancelled_copies);
}
TEST(ActorCoroutineScope, QB267PublicNestedMiddleDropLeavesGrandchildIndependent) {
    check_topology(Scenario::nested_drop);
}
TEST(ActorCoroutineScope, QB267PublicReentrantCancelCanDropSiblingAndCreateCancelledChild) {
    check_topology(Scenario::reentrant);
}
} // namespace coroutine_scope_qb267_topology_test
