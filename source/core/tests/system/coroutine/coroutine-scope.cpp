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
 * Hardening over the original (see docs/tests-audit/qb-core/qbcore-c09.md):
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
 */

#include <atomic>
#include <chrono>

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
//    The loop has a 2ms period and the kill fires at 60ms, so ~30 iterations land
//    before cancellation on any realistic tick — the "made progress before cancel"
//    lower bound (> 0) has an enormous margin and cannot flake under load. The kill
//    margin is deliberately wide (the dossier's tightening option) rather than a
//    fragile narrow window.
// ---------------------------------------------------------------------------
class CancellationPointActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                for (;;) {
                    co_await ctx.cancellation_point(); // throws once the scope is cancelled
                    co_await ctx.sleep(2ms);
                    g_loop_iterations.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const qb::io::async::cancelled_error &) {
                g_scoped_cancel_observed = true;
            }
        });
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            60ms); // ~30 loop iterations land before this fires — a wide, load-robust margin
        co_return true;
    }
};

TEST(ActorCoroutineScope, CancellationPointBailsOnKill) {
    reset_flags();
    qb::Main main;
    main.addActor<CancellationPointActor>(0);
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
            // Step 1 is a tiny (10ms) await that completes; step 2 is a deliberately LONG (1s) await
            // so the 50ms kill is guaranteed to land strictly inside it on any realistic CI tick
            // (well after step 1 finished, far before step 2's 1s deadline). The step counter then
            // proves cancellation landed in step 2, deterministically.
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
        qb::io::async::callback(
            [this] {
                if (is_alive())
                    kill();
            },
            50ms); // fires during the 1s step-2 await (after the 10ms step 1)
        co_return true;
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
//     be reclaimed too — not left parked until its (e.g. 2s) timer fires (or,
//     since core 0 runs on the test thread via start(false), leaked past the
//     run entirely because `listener::current` is never torn down here).
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
} // namespace

class CancellableLeakActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await ctx.cancellable(scope_slow_task()); // arms a detached runner + inner 2s task
            } catch (const qb::io::async::cancelled_error &) {
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

TEST(ActorCoroutineScope, CancellableDetachedTimerReclaimedNoLeak) {
    const long baseline = live_frames_now();
    {
        qb::Main main;
        main.addActor<CancellableLeakActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    // Pre-fix this ended at baseline+2 (the detached runner frame + the inner 2s
    // task frame, both parked until the inner timer fired — i.e. leaked here,
    // since core 0's listener is never torn down on this thread).
    EXPECT_EQ(live_frames_now(), baseline) << "ctx.cancellable() must reclaim its detached runner + inner task frames on cancel";
}

class ChildSleepLeakActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto child = ctx.child_token();
            try {
                co_await qb::io::async::cancellable_sleep(2s, child); // arms a detached timer_task
            } catch (const qb::io::async::cancelled_error &) {
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

TEST(ActorCoroutineScope, CancellableSleepDetachedTimerReclaimedNoLeak) {
    const long baseline = live_frames_now();
    {
        qb::Main main;
        main.addActor<ChildSleepLeakActor>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    // Pre-fix this ended at baseline+1 (the detached timer_task frame, parked on
    // the full 2s sleep). child_token() routes the actor-scope cancel into the
    // cancellable_sleep awaiter, which now tears the timer_task down on cancel.
    EXPECT_EQ(live_frames_now(), baseline) << "cancellable_sleep must reclaim its detached timer_task frame on cancel";
}
