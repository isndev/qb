/**
 * @file unit/coroutine/scheduler-lifecycle.cpp
 * @brief `qb::io::async::CoroutineScheduler` + `task<T>` lifecycle — the canonical scheduler/ownership file.
 *
 * This is the deterministic ground-truth for how the coroutine scheduler owns, runs, tracks and frees
 * frames. Everything here drives the in-process event loop — `qb::io::async::init()` (via
 * `reset_async_context()` in SetUp), `spawn()`, `run_ready()` and the de-flake pump `pump_until()` — with
 * NO sockets, NO daemon, NO TLS, so it is a pure `unit` test. The contracts proven:
 *
 *   - task<T> pre-spawn handle state: a freshly built task is truthy and not done; after spawn() the
 *     handle is transferred and the moved-from task is empty (move-only semantics).
 *   - ownership transfer: a spawned coroutine runs to completion even after the originating task object
 *     dies, and `active_count()` returns to 0 once it finishes (no frame / bookkeeping leak).
 *   - frame accounting: a spawned coroutine that SUSPENDS then completes via symmetric transfer is freed
 *     through the final_suspend defer-destruction path (`live_frames` returns to baseline) — the historic
 *     spawn-then-await frame leak regression.
 *   - in-flight tracking: `active_count()` is stepped deterministically with `run_ready()` (no sleeps) to
 *     show it counts ready+suspended frames and drains to 0.
 *   - TLS scheduler identity survives a listener reset and is always reachable through `current_ptr()`.
 *   - unhandled-exception path tears the coroutine down cleanly and leaves the scheduler usable.
 *   - symmetric transfer keeps a deep await chain off the C++ stack.
 *
 * Restructured from the dissolved test-coroutine-basic.cpp (TaskIsCreatedAndExecutes pre-spawn state,
 * SpawnedTaskContinuesAfterHandleReleased) and test-coroutine-regression.cpp
 * (SchedulerCurrentPtrReflectsListenerReset) folded in; the former wall-clock `run_for(Nms)` budgets are
 * replaced by `qb::io::test::pump_until` so a stalled coroutine fails loudly instead of racing or hanging;
 * the two `EXPECT_TRUE(x||!x)` tautology "cancellation" tests are replaced with the actual defined
 * contracts (spawned coroutine continues after task-drop; awaited-task exception surfaces); the disabled
 * SchedulerTracksInFlightCoroutines is resurrected deterministically via run_ready() stepping and the
 * unfireable DISABLED_ExceptionDuringValueReturn is dropped. No file-local main(): shared gtest_main.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

// =============================================================================
// TEST FIXTURE
// =============================================================================

class CoroutineSchedulerTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }

    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

// =============================================================================
// TASK PRE-SPAWN STATE & OWNERSHIP TRANSFER
// =============================================================================

/**
 * @test Task created, spawned, and executed
 * @brief A fresh task is truthy + not-done before spawn; spawn() moves it out and runs it to completion.
 *
 * Salvaged from the dissolved test-coroutine-basic.cpp::TaskIsCreatedAndExecutes — the pre-spawn
 * handle-state assertion was underrepresented elsewhere. The poll/flag wall-clock loop is replaced by
 * pump_until, and the moved-from emptiness is asserted explicitly.
 */
TEST_F(CoroutineSchedulerTests, TaskIsCreatedAndExecutes) {
    std::atomic<bool> executed{false};

    auto coro_fn = [&executed]() -> task<void> {
        executed.store(true, std::memory_order_release);
        co_return;
    };
    auto t = coro_fn();

    // Pre-spawn: the task owns a live, not-yet-run frame.
    EXPECT_TRUE(static_cast<bool>(t));
    EXPECT_FALSE(t.done());

    coro_scheduler().spawn(std::move(t));

    // spawn() transferred the handle: the moved-from task is now empty.
    EXPECT_FALSE(static_cast<bool>(t));

    EXPECT_TRUE(pump_until([&] { return executed.load(std::memory_order_acquire); }))
        << "spawned coroutine never executed";
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

/**
 * @test Spawned coroutine continues after the task handle is released
 * @brief spawn() takes ownership — dropping the originating task does NOT cancel the running coroutine;
 *        it completes and the scheduler bookkeeping drains to zero.
 *
 * Salvaged from test-coroutine-basic.cpp::SpawnedTaskContinuesAfterHandleReleased (the active_count()==0
 * cleanup assertion is the value-add). This is the CORRECTLY-NAMED replacement for the dissolved
 * comprehensive.cpp::CoroutineDestructionCancelsOperation, whose body asserted the opposite of its name.
 */
TEST_F(CoroutineSchedulerTests, SpawnedCoroutineContinuesAfterHandleReleased) {
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};

    {
        auto started_ptr   = &started;
        auto completed_ptr = &completed;
        auto coro_fn       = [started_ptr, completed_ptr]() -> task<void> {
            started_ptr->store(true);
            co_await sleep(20ms);
            completed_ptr->store(true);
            co_return;
        };
        // Owned-callable overload: the closure dies at the end of this block while
        // the coroutine keeps running afterwards (the frame owns a copy).
        coro_scheduler().spawn(coro_fn);

        // It must have started but not yet completed (still parked on the timer).
        EXPECT_TRUE(pump_until([&] { return started.load(); })) << "coroutine never started";
        EXPECT_FALSE(completed.load());
        // task/closure goes out of scope here — must NOT cancel the spawned coroutine.
    }

    EXPECT_TRUE(pump_until([&] { return completed.load(); }))
        << "spawned coroutine was cancelled when its originating closure was released";
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

/**
 * @test task<T> is move-only with correct moved-from state
 * @brief Compile-time + runtime move-only semantics; the moved-from task is empty.
 */
TEST_F(CoroutineSchedulerTests, TaskIsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<task<int>>);
    static_assert(!std::is_copy_assignable_v<task<int>>);
    static_assert(std::is_move_constructible_v<task<int>>);
    static_assert(std::is_move_assignable_v<task<int>>);

    auto fn = []() -> task<int> {
        co_return 42;
    };

    auto t1 = fn();
    auto t2 = std::move(t1);

    EXPECT_FALSE(t1.handle());
    EXPECT_TRUE(t2.handle());
    EXPECT_FALSE(static_cast<bool>(t1));
    EXPECT_TRUE(static_cast<bool>(t2));
}

/**
 * @test Move-only return value travels through co_await
 * @brief A coroutine can return a move-only type; the awaiter receives the exact value.
 */
TEST_F(CoroutineSchedulerTests, MoveOnlyReturnValue) {
    struct MoveOnly {
        std::unique_ptr<int> data;
        explicit MoveOnly(int val)
            : data(std::make_unique<int>(val)) {}
        MoveOnly(const MoveOnly &) = delete;
        MoveOnly(MoveOnly &&)      = default;
    };

    std::atomic<int> result{-1};

    auto producer_fn = []() -> task<MoveOnly> {
        co_await sleep(5ms);
        co_return MoveOnly{42};
    };

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr, producer_fn]() -> task<void> {
        auto obj = co_await producer_fn();
        result_ptr->store(obj.data ? *obj.data : -2);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return result.load() != -1; })) << "producer never delivered value";
    EXPECT_EQ(result.load(), 42);
}

// =============================================================================
// FRAME LIFECYCLE / LEAK REGRESSIONS
// =============================================================================

/**
 * @test Spawned suspending frame is freed (regression)
 * @brief A spawned coroutine that suspends then completes via symmetric transfer must free its frame.
 *
 * It completes via symmetric transfer from its awaited inner task, so its own handle is never re-examined
 * by run_ready(); the scheduler instead destroys it through the final_suspend defer-destruction path.
 * Before that fix every such spawn leaked exactly one frame. `live_frames` is the pooled-allocator's
 * per-thread live-frame counter (qb/io/async/coroutine/task.h).
 */
TEST_F(CoroutineSchedulerTests, SpawnedSuspendingFrameIsFreed) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto done_ptr = &done;
    auto fn       = [done_ptr]() -> task<void> {
        co_await sleep(5ms);
        done_ptr->store(true);
    };
    coro_scheduler().spawn(fn);

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";

    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "leaked " << (after - baseline) << " coroutine frame(s)";
}

/**
 * @test Coroutine frame (and its locals) are destroyed after completion
 * @brief Verify the frame is alive while parked and torn down (running the local's dtor) once it returns.
 */
TEST_F(CoroutineSchedulerTests, FrameDestroyedAfterCompletion) {
    struct FrameMarker {
        std::atomic<bool> *destroyed;
        explicit FrameMarker(std::atomic<bool> *d)
            : destroyed(d) {}
        ~FrameMarker() {
            if (destroyed)
                destroyed->store(true);
        }
    };

    std::atomic<bool> frame_destroyed{false};
    std::atomic<bool> completed{false};

    auto destroyed_ptr = &frame_destroyed;
    auto completed_ptr = &completed;
    coro_scheduler().spawn([destroyed_ptr, completed_ptr]() -> task<void> {
        FrameMarker marker{destroyed_ptr};
        co_await sleep(10ms);
        completed_ptr->store(true);
        co_return;
        // marker destroyed here, as part of frame teardown
    });

    // While parked on the timer the frame (hence the local) is still alive.
    qb::io::async::run_for(2ms);
    EXPECT_FALSE(completed.load());
    EXPECT_FALSE(frame_destroyed.load());

    EXPECT_TRUE(pump_until([&] { return completed.load(); })) << "coroutine never completed";
    EXPECT_TRUE(frame_destroyed.load()) << "frame local was not destroyed after completion";
}

/**
 * @test Task destroyed before spawn never runs
 * @brief A task that is built and dropped without spawning must not execute, and leaves no scheduler state.
 */
TEST_F(CoroutineSchedulerTests, TaskDestroyedBeforeSpawnNeverRuns) {
    std::atomic<bool> executed{false};

    {
        auto executed_ptr = &executed;
        auto fn           = [executed_ptr]() -> task<void> {
            executed_ptr->store(true);
            co_return;
        };
        auto t = fn();
        (void) t; // destroyed here, never spawned
    }

    // Give the loop a bounded chance to (wrongly) run anything queued.
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(executed.load());
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

// =============================================================================
// IN-FLIGHT TRACKING (deterministic, no sleeps)
// =============================================================================

/**
 * @test Scheduler tracks in-flight + suspended coroutines
 * @brief active_count() counts ready+suspended frames and drains to 0; stepped via run_ready() so the
 *        observation points are deterministic rather than wall-clock races.
 *
 * Resurrected from the dissolved DISABLED_SchedulerTracksInFlightCoroutines. The original was disabled
 * for being timing-sensitive; here the scheduler is stepped explicitly: after spawn the frames are ready
 * (in the queue), after one run_ready() step they are suspended on their timers (still counted), and
 * after the timers fire and the bodies complete the count is exactly 0.
 */
TEST_F(CoroutineSchedulerTests, SchedulerTracksInFlightCoroutines) {
    auto &sched = coro_scheduler();
    EXPECT_EQ(sched.active_count(), 0u);

    constexpr int    kCount = 3;
    std::atomic<int> completed{0};

    auto completed_ptr = &completed;
    for (int i = 0; i < kCount; ++i) {
        auto fn = [completed_ptr]() -> task<void> {
            co_await sleep(10ms);
            completed_ptr->fetch_add(1);
            co_return;
        };
        sched.spawn(fn);
    }

    // Freshly spawned: all three frames are ready in the queue, none suspended yet.
    EXPECT_EQ(sched.active_count(), static_cast<std::size_t>(kCount));
    EXPECT_EQ(completed.load(), 0);

    // One step runs each body up to its first co_await sleep — now all three are suspended,
    // still alive, still counted, and none has completed.
    sched.run_ready();
    EXPECT_EQ(sched.active_count(), static_cast<std::size_t>(kCount));
    EXPECT_EQ(completed.load(), 0);

    // Drive the timers to completion: every coroutine finishes and the scheduler drains.
    EXPECT_TRUE(pump_until([&] { return completed.load() == kCount; })) << "not all coroutines completed";
    EXPECT_EQ(sched.active_count(), 0u);
}

/**
 * @test Ready queue processes every spawned coroutine
 * @brief A burst of immediately-ready coroutines all execute on one drain.
 */
TEST_F(CoroutineSchedulerTests, ReadyQueueProcessesAllCoroutines) {
    constexpr int    kCount = 5;
    std::atomic<int> execution_count{0};

    auto count_ptr = &execution_count;
    for (int i = 0; i < kCount; ++i) {
        auto fn = [count_ptr]() -> task<void> {
            count_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(fn);
    }

    coro_scheduler().run_ready();
    EXPECT_EQ(execution_count.load(), kCount);
}

/**
 * @test pending_count() reflects the ready queue across the run cycle
 * @brief Empty initially; exactly 1 after spawning a suspending coroutine (its initial-suspend resume);
 *        empties once the coroutine parks on its timer.
 *
 * Determinised from the dissolved basic.cpp::SchedulerStateTracking smoke test (which asserted nothing
 * after run_ready).
 */
TEST_F(CoroutineSchedulerTests, PendingCountReflectsReadyQueue) {
    auto &sched = coro_scheduler();

    EXPECT_FALSE(sched.has_ready());
    EXPECT_EQ(sched.pending_count(), 0u);

    auto fn = []() -> task<void> {
        co_await sleep(50ms);
        co_return;
    };
    sched.spawn(fn);

    // The spawned coroutine is queued for its initial resume.
    EXPECT_TRUE(sched.has_ready());
    EXPECT_EQ(sched.pending_count(), 1u);

    // After one drain it suspends on the sleep watcher; the ready queue empties.
    sched.run_ready();
    EXPECT_EQ(sched.pending_count(), 0u);
    EXPECT_FALSE(sched.has_ready());
}

// =============================================================================
// HANDLE / COMPOSITION CONTRACTS
// =============================================================================

/**
 * @test Awaiting an already-completed inner task returns synchronously
 * @brief task::await_ready() short-circuits a finished inner task; the value is delivered.
 */
TEST_F(CoroutineSchedulerTests, AwaitingCompletedTaskReturnsValue) {
    std::atomic<int> result{-1};

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        auto inner_fn = []() -> task<int> {
            co_return 42;
        };
        int val = co_await inner_fn();
        result_ptr->store(val);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return result.load() != -1; })) << "awaiter never resumed";
    EXPECT_EQ(result.load(), 42);
}

/**
 * @test Promise is_ready() reflects completion state across an await
 * @brief Before awaiting an unfinished inner task is_ready() is false; after the await the value is bound.
 */
TEST_F(CoroutineSchedulerTests, PromiseIsReadyReflectsState) {
    std::atomic<bool> checked{false};
    std::atomic<int>  value{-1};

    auto checked_ptr = &checked;
    auto value_ptr   = &value;
    coro_scheduler().spawn([checked_ptr, value_ptr]() -> task<void> {
        auto inner_fn = []() -> task<int> {
            co_await sleep(10ms);
            co_return 42;
        };
        auto inner = inner_fn();
        EXPECT_FALSE(inner.handle().promise().is_ready());

        int val = co_await inner;
        value_ptr->store(val);
        checked_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return checked.load(); })) << "coroutine never completed";
    EXPECT_EQ(value.load(), 42);
}

// =============================================================================
// EXCEPTION EDGE CASES (scheduler-level; full propagation matrix in exception-propagation.cpp)
// =============================================================================

/**
 * @test Unhandled exception in a spawned root tears down cleanly
 * @brief A throwing spawned coroutine stops at the throw, runs nothing after it, and the scheduler is
 *        left empty and usable.
 */
TEST_F(CoroutineSchedulerTests, UnhandledExceptionTerminatesCoroutine) {
    std::atomic<bool> after_throw{false};

    auto after_ptr = &after_throw;
    coro_scheduler().spawn([after_ptr]() -> task<void> {
        throw std::runtime_error("unhandled");
        after_ptr->store(true); // unreachable
        co_return;
    });

    // The unhandled exception is captured in the promise; the spawned frame completes and is reclaimed.
    EXPECT_TRUE(pump_until([&] { return coro_scheduler().active_count() == 0u; }))
        << "scheduler never drained the throwing coroutine";
    EXPECT_FALSE(after_throw.load());
}

/**
 * @test Exception from an awaited task surfaces at the co_await
 * @brief Replaces the old vacuous CancellationOfAwaitedTask (which asserted nothing about the outcome):
 *        an inner task that throws after suspension propagates the exception to the awaiting coroutine.
 */
TEST_F(CoroutineSchedulerTests, ExceptionFromAwaitedTaskPropagates) {
    std::atomic<bool> inner_started{false};
    std::atomic<bool> caught{false};
    std::atomic<bool> outer_continued{false};

    auto inner_ptr = &inner_started;
    auto caught_ptr = &caught;
    auto outer_ptr  = &outer_continued;
    coro_scheduler().spawn([inner_ptr, caught_ptr, outer_ptr]() -> task<void> {
        auto inner_fn = [inner_ptr]() -> task<int> {
            inner_ptr->store(true);
            co_await sleep(10ms);
            throw std::runtime_error("inner failed");
            co_return 42;
        };
        try {
            int val = co_await inner_fn();
            (void) val;
        } catch (const std::runtime_error &) {
            caught_ptr->store(true);
        }
        outer_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return outer_continued.load(); })) << "outer coroutine never resumed";
    EXPECT_TRUE(inner_started.load());
    EXPECT_TRUE(caught.load()) << "awaited-task exception did not surface at the co_await";
}

// =============================================================================
// SYMMETRIC TRANSFER
// =============================================================================

/**
 * @test Deep await chain does not overflow the stack
 * @brief Symmetric transfer keeps a 100-deep recursive await chain off the C++ call stack and returns
 *        the exact accumulated value.
 */
TEST_F(CoroutineSchedulerTests, DeepChainNoStackOverflow) {
    constexpr int    kDepth = 100;
    std::atomic<int> final_result{-1};

    std::function<task<int>(int)> chain = [&chain](int n) -> task<int> {
        if (n == 0) {
            co_return 0;
        }
        int val = co_await chain(n - 1);
        co_return val + 1;
    };

    auto result_ptr = &final_result;
    coro_scheduler().spawn([&chain, result_ptr]() -> task<void> {
        int v = co_await chain(kDepth);
        result_ptr->store(v);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return final_result.load() != -1; })) << "deep chain never completed";
    EXPECT_EQ(final_result.load(), kDepth);
}

/**
 * @test Symmetric transfer with immediate (no-suspend) completion
 * @brief An inner task that completes without suspending hands control straight back; observe the exact
 *        interleaved execution order via additive markers.
 */
TEST_F(CoroutineSchedulerTests, SymmetricTransferImmediateCompletion) {
    std::atomic<int> execution_count{0};

    auto count_ptr = &execution_count;
    coro_scheduler().spawn([count_ptr]() -> task<void> {
        auto inner_fn = [count_ptr]() -> task<int> {
            count_ptr->fetch_add(1);
            co_return 42;
        };
        count_ptr->fetch_add(10);
        int val = co_await inner_fn();
        EXPECT_EQ(val, 42);
        count_ptr->fetch_add(100);
        co_return;
    });

    coro_scheduler().run_ready();
    // outer(10) + inner(1) + outer(100) = 111, all on a single drain (no real suspension).
    EXPECT_EQ(execution_count.load(), 111);
}

// =============================================================================
// TLS SCHEDULER IDENTITY
// =============================================================================

/**
 * @test Each thread has its own scheduler instance
 * @brief The TLS scheduler is per-thread; a worker thread observes a different instance.
 */
TEST_F(CoroutineSchedulerTests, SchedulerIsThreadLocal) {
    auto *main_sched = &coro_scheduler();

    std::atomic<void *> thread_sched_addr{nullptr};
    auto                addr_ptr = &thread_sched_addr;

    std::thread t([addr_ptr]() {
        qb::io::async::init();
        addr_ptr->store(&coro_scheduler());
    });
    t.join();

    EXPECT_NE(thread_sched_addr.load(), nullptr);
    EXPECT_NE(main_sched, thread_sched_addr.load());
}

/**
 * @test current_ptr() reflects a listener reset (regression — Finding 2.D.4)
 * @brief After resetting the TLS scheduler, current_ptr() is null until the next access, and the freshly
 *        created scheduler is the one reachable through current_ptr().
 *
 * Salvaged verbatim-in-spirit from the dissolved
 * test-coroutine-regression.cpp::SchedulerCurrentPtrReflectsListenerReset: Actor::spawn_detached caches a
 * scheduler pointer and must revalidate it against the current TLS scheduler; this drives the scheduler
 * directly to prove the underlying invariant.
 *
 * NOTE: drives the TLS scheduler lifecycle directly, so it deliberately does NOT use the fixture's
 * reset_coro_scheduler-based TearDown assumptions mid-test — it re-creates the scheduler before returning.
 */
TEST_F(CoroutineSchedulerTests, SchedulerCurrentPtrReflectsListenerReset) {
    auto *before = &qb::io::async::listener::current.coro_scheduler();
    ASSERT_EQ(before, CoroutineScheduler::current_ptr());

    qb::io::async::listener::current.reset_coro_scheduler();
    EXPECT_EQ(CoroutineScheduler::current_ptr(), nullptr);

    auto *after = &qb::io::async::listener::current.coro_scheduler();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after, CoroutineScheduler::current_ptr());
    EXPECT_EQ(after, &qb::io::async::listener::current.coro_scheduler());
}

// =============================================================================
// READY-QUEUE STRESS
// =============================================================================

/**
 * @test Many spawns stress the ready queue
 * @brief 200 short-lived coroutines all complete; the mono-thread deque ready queue handles the churn.
 */
TEST_F(CoroutineSchedulerTests, ManySpawnsStressReadyQueue) {
    constexpr int    kN = 200;
    std::atomic<int> done{0};

    auto done_ptr = &done;
    for (int i = 0; i < kN; ++i) {
        auto fn = [done_ptr]() -> task<void> {
            co_await sleep(1ms);
            done_ptr->fetch_add(1);
        };
        coro_scheduler().spawn(fn);
    }

    EXPECT_TRUE(pump_until([&] { return done.load() == kN; })) << "not all spawned coroutines completed";
    EXPECT_EQ(done.load(), kN);
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

/**
 * @test Burst of suspend/resume cycles stresses schedule_resume
 * @brief 100 coroutines each suspend/resume 5 times; every step is accounted for.
 */
TEST_F(CoroutineSchedulerTests, BurstScheduleResumeStress) {
    constexpr int    kN = 100;
    std::atomic<int> steps{0};

    auto steps_ptr = &steps;
    auto fn        = [steps_ptr]() -> task<void> {
        for (int i = 0; i < 5; ++i) {
            co_await sleep(2ms);
            steps_ptr->fetch_add(1);
        }
    };

    for (int i = 0; i < kN; ++i) {
        coro_scheduler().spawn(fn);
    }

    EXPECT_TRUE(pump_until([&] { return steps.load() == kN * 5; })) << "not all suspend/resume steps ran";
    EXPECT_EQ(steps.load(), kN * 5);
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}
