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

// =============================================================================
// SCHEDULER BOOKKEEPING EDGE CASES (forget / dedup / null-handle / re-entrancy)
// =============================================================================

/**
 * @test schedule_resume / enqueue_for_later dedup the same handle
 * @brief A handle already in-flight is not queued a second time (scheduler.h:402 / :428 dedup branches).
 *        A double schedule_resume + double enqueue_for_later of one live, parked frame must leave the
 *        ready queue holding exactly one entry for it — the single-resume coroutine contract.
 */
TEST_F(CoroutineSchedulerTests, ScheduleResumeAndEnqueueDedup) {
    auto &sched = coro_scheduler();

    std::atomic<int> resumes{0};
    std::atomic<int> stage{0};

    // Spawn a coroutine that parks on a long sleep so we own a live, suspended handle to poke.
    auto resumes_ptr = &resumes;
    auto stage_ptr   = &stage;
    coro_scheduler().spawn([resumes_ptr, stage_ptr]() -> task<void> {
        stage_ptr->store(1);
        co_await sleep(500ms); // park; we will resume it manually below
        resumes_ptr->fetch_add(1);
        stage_ptr->store(2);
        co_return;
    });

    // Run one step so the body reaches the sleep and suspends (now tracked as suspended, not in-flight).
    sched.run_ready();
    ASSERT_EQ(stage.load(), 1);
    EXPECT_EQ(sched.pending_count(), 0u);

    // Reach the suspended frame's handle through the scheduler is not exposed; instead exercise the
    // dedup on a fresh, manually-built handle that we keep parked at initial_suspend.
    auto manual = [stage_ptr]() -> task<void> {
        co_await std::suspend_always{}; // never resumes on its own
        stage_ptr->fetch_add(1000);
    }();
    auto h = manual.handle();
    ASSERT_TRUE(h);

    // First schedule_resume queues it; the second hits the in_flight_ dedup and is a no-op.
    sched.schedule_resume(h);
    EXPECT_EQ(sched.pending_count(), 1u);
    sched.schedule_resume(h);
    EXPECT_EQ(sched.pending_count(), 1u) << "schedule_resume failed to dedup an in-flight handle";

    // enqueue_for_later for an already-in-flight handle also dedups (scheduler.h:428).
    sched.enqueue_for_later(h);
    EXPECT_EQ(sched.pending_count(), 1u) << "enqueue_for_later failed to dedup an in-flight handle";

    // forget() scrubs it from the ready queue + in-flight set so run_ready() never resumes it.
    sched.forget(h);
    EXPECT_EQ(sched.pending_count(), 0u) << "forget did not remove the in-flight handle from the ready queue";

    // The manual frame was never resumed (still parked at initial suspend). forget() did not free
    // it — `manual` still owns the frame and destroys it exactly once in its destructor at scope
    // end. (A manual h.destroy() here would double-free and corrupt the frame-allocator free-list.)
    EXPECT_EQ(stage.load(), 1); // the +1000 body never ran
}

/**
 * @test schedule_resume / enqueue_for_later reject null and done handles
 * @brief The early-return guards (scheduler.h:399 / :419) — a null or completed handle is never queued.
 */
TEST_F(CoroutineSchedulerTests, ScheduleAndEnqueueRejectNullAndDone) {
    auto &sched = coro_scheduler();
    ASSERT_EQ(sched.pending_count(), 0u);

    // Null handles: both guards bail before touching the queue.
    sched.schedule_resume(std::coroutine_handle<>{});
    sched.enqueue_for_later(std::coroutine_handle<>{});
    EXPECT_EQ(sched.pending_count(), 0u);

    // A completed (done) handle: build a trivial task, run it to done, then verify it is refused.
    auto t = []() -> task<void> { co_return; }();
    auto h = t.handle();
    h.resume(); // runs the body to completion; handle is now done()
    ASSERT_TRUE(h.done());
    sched.schedule_resume(h);
    sched.enqueue_for_later(h);
    EXPECT_EQ(sched.pending_count(), 0u) << "a done handle must never be queued";
    // task `t` destroys the (done) frame in its destructor.
}

/**
 * @test forget() on an unknown / null handle is a harmless no-op
 * @brief forget(null) returns immediately (scheduler.h:333); forget() of a handle the scheduler never
 *        tracked leaves all bookkeeping untouched.
 */
TEST_F(CoroutineSchedulerTests, ForgetUnknownHandleIsNoOp) {
    auto &sched = coro_scheduler();

    sched.forget(std::coroutine_handle<>{}); // null guard
    EXPECT_EQ(sched.active_count(), 0u);

    // A live, never-scheduled frame: forget() finds it in none of the sets and changes nothing.
    // forget() never frees — `t` still owns the frame and destroys it exactly once in its
    // destructor at scope end. (Calling h.destroy() here as well would double-free the frame
    // and corrupt the thread-local CoroutineFrameAllocator free-list.)
    auto t = []() -> task<void> { co_await std::suspend_always{}; }();
    auto h = t.handle();
    sched.forget(h);
    EXPECT_EQ(sched.active_count(), 0u);
}

/**
 * @test register_suspended / unregister_suspended ignore a null handle
 * @brief Both null guards (scheduler.h:624 / :640) — calling with an empty handle must not change the
 *        suspended count.
 */
TEST_F(CoroutineSchedulerTests, RegisterSuspendedNullHandleIgnored) {
    auto &sched = coro_scheduler();
    const auto before = sched.active_count();

    sched.register_suspended(std::coroutine_handle<>{});
    sched.unregister_suspended(std::coroutine_handle<>{});

    EXPECT_EQ(sched.active_count(), before) << "null handle wrongly mutated the suspended set";
}

/**
 * @test run_ready() refuses re-entrant invocation
 * @brief The re-entrancy guard (scheduler.h:464-473) returns 0 when run_ready() is called from inside a
 *        coroutine that is itself running under a run_ready() drain. In a debug build the assert fires;
 *        this test only runs in NDEBUG builds where the guard returns 0 silently (strictly safer path).
 */
#ifdef NDEBUG
TEST_F(CoroutineSchedulerTests, RunReadyRejectsReentrantCall) {
    auto &sched = coro_scheduler();

    std::atomic<std::size_t> nested_return{1}; // sentinel != 0 so we can see it was written
    std::atomic<bool>        ran{false};

    auto nested_ptr = &nested_return;
    auto ran_ptr    = &ran;
    coro_scheduler().spawn([nested_ptr, ran_ptr]() -> task<void> {
        // Calling run_ready() while we are already inside the outer run_ready() drain must be refused.
        nested_ptr->store(coro_scheduler().run_ready());
        ran_ptr->store(true);
        co_return;
    });

    sched.run_ready();
    EXPECT_TRUE(ran.load());
    EXPECT_EQ(nested_return.load(), 0u) << "re-entrant run_ready() must return 0";
}
#endif

/**
 * @test is_draining_ready() reflects the run_ready() window
 * @brief Outside a drain it is false; inside a running coroutine body (which runs under run_ready) it is
 *        true (scheduler.h:546 / in_run_ready_ flag set by the ReentrancyGuard).
 */
TEST_F(CoroutineSchedulerTests, IsDrainingReadyReflectsRunReadyWindow) {
    auto &sched = coro_scheduler();
    EXPECT_FALSE(sched.is_draining_ready());

    std::atomic<bool> saw_draining{false};
    auto saw_ptr = &saw_draining;
    coro_scheduler().spawn([saw_ptr]() -> task<void> {
        saw_ptr->store(coro_scheduler().is_draining_ready());
        co_return;
    });

    sched.run_ready();
    EXPECT_TRUE(saw_draining.load()) << "is_draining_ready() should be true inside a run_ready() drain";
    EXPECT_FALSE(sched.is_draining_ready()) << "draining flag must clear after run_ready() returns";
}

// =============================================================================
// SPAWN_TRACKED / CANCEL_SPAWNED / DESTROY_ALL_SUSPENDED (helper lifecycle)
// =============================================================================

/**
 * @test spawn_tracked() returns an empty handle for an empty task
 * @brief The early-return when detach() yields no handle (scheduler.h:858-859).
 */
TEST_F(CoroutineSchedulerTests, SpawnTrackedEmptyTaskReturnsNull) {
    auto &sched = coro_scheduler();

    task<void> empty{}; // default-constructed: no frame
    ASSERT_FALSE(static_cast<bool>(empty));

    auto h = sched.spawn_tracked(std::move(empty));
    EXPECT_FALSE(h) << "spawn_tracked of an empty task must return an empty handle";
    EXPECT_EQ(sched.active_count(), 0u);
}

/**
 * @test spawn_tracked() + cancel_spawned() reclaims a parked helper frame
 * @brief spawn_tracked owns the frame; cancel_spawned destroys it early while it is parked on a long
 *        timer (scheduler.h:271-301), stopping its watcher and dropping it from active_count without
 *        waiting for the timer. Drives the cancel_spawned ownership-gate + bookkeeping-scrub path.
 */
TEST_F(CoroutineSchedulerTests, SpawnTrackedThenCancelReclaimsHelper) {
    auto &sched = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> ran_past_sleep{false};
    auto ran_ptr = &ran_past_sleep;

    // A helper that parks on a long timer; if it ever fires it flips the flag (it must NOT).
    auto make_helper = [ran_ptr]() -> task<void> {
        co_await sleep(2000ms);
        ran_ptr->store(true);
    };
    auto h = sched.spawn_tracked(make_helper());
    ASSERT_TRUE(h);

    // Step once so the helper reaches its sleep and parks (suspended + watcher armed).
    sched.run_ready();
    EXPECT_EQ(sched.active_count(), 1u) << "helper should be parked and counted";

    // Cancel it: owned frame is destroyed, watcher stopped, bookkeeping scrubbed.
    sched.cancel_spawned(h);
    EXPECT_EQ(sched.active_count(), 0u) << "cancel_spawned did not reclaim the parked helper";

    // A second cancel of the now-stale handle hits the ownership gate (erase==0) and is a safe no-op.
    sched.cancel_spawned(h);
    EXPECT_EQ(sched.active_count(), 0u);

    // Give the loop a chance: the cancelled timer must never fire.
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(ran_past_sleep.load()) << "cancelled helper's timer wrongly fired";

    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "cancel_spawned leaked " << (after - baseline) << " frame(s)";
}

/**
 * @test cancel_spawned() on a null or non-owned handle is a no-op
 * @brief The null guard (scheduler.h:272-273) and the ownership gate (scheduler.h:278-279): a handle the
 *        scheduler never owned is never destroyed.
 */
TEST_F(CoroutineSchedulerTests, CancelSpawnedNullAndNonOwnedNoOp) {
    auto &sched = coro_scheduler();

    sched.cancel_spawned(std::coroutine_handle<>{}); // null guard
    EXPECT_EQ(sched.active_count(), 0u);

    // A live frame the scheduler does not own: cancel_spawned must NOT free it (ownership gate).
    auto t = []() -> task<void> { co_await std::suspend_always{}; }();
    auto h = t.handle();
    sched.cancel_spawned(h);
    EXPECT_EQ(sched.active_count(), 0u);
    // `t` still owns the live frame; cancel_spawned left it untouched, so `t` remains valid and
    // destroys the frame exactly once in its destructor at scope end. (A manual h.destroy() here
    // would double-free the still-owned frame and corrupt the frame-allocator free-list.)
    EXPECT_TRUE(static_cast<bool>(t));
}

/**
 * @test destroy_all_suspended() tears down a parked owned-root + a non-owned suspended chain
 * @brief Drives both passes of destroy_all_suspended (scheduler.h:652-703): the owned-root cascade (a
 *        spawn_tracked helper parked on a non-cancellable sleep) and the trailing non-owned suspended
 *        sweep. After it runs, active_count() is 0 and no frame leaked, with the loop still valid.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedTearsDownMixedFrames) {
    auto &sched = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<int> fired{0};
    auto fired_ptr = &fired;

    // (a) Owned root: spawn_tracked helper that parks on a long plain sleep (cannot be woken by cancel).
    auto owned = [fired_ptr]() -> task<void> {
        co_await sleep(3000ms);
        fired_ptr->fetch_add(1);
    };
    auto owned_h = sched.spawn_tracked(owned());
    ASSERT_TRUE(owned_h);

    // (b) Non-owned suspended frame: a plain spawn() coroutine also parked on a long sleep.
    auto plain = [fired_ptr]() -> task<void> {
        co_await sleep(3000ms);
        fired_ptr->fetch_add(1);
    };
    coro_scheduler().spawn(plain);

    // Step so both reach their sleeps and are parked/suspended.
    sched.run_ready();
    EXPECT_GE(sched.active_count(), 1u);

    // Nuke everything: both the owned-root cascade and the suspended sweep fire.
    sched.destroy_all_suspended();
    EXPECT_EQ(sched.active_count(), 0u) << "destroy_all_suspended left frames parked";

    // The timers were stopped by the awaiter destructors — neither body ever completes.
    qb::io::async::run_for(10ms);
    EXPECT_EQ(fired.load(), 0) << "a destroyed coroutine's timer wrongly fired";

    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "destroy_all_suspended leaked " << (after - baseline) << " frame(s)";
}

/**
 * @test defer_destroy() drains on the next run_ready()
 * @brief A spawned coroutine that completes via symmetric transfer hands its frame to defer_destroy
 *        (scheduler.h:712-716), which run_ready() then frees on its deferred-destroy drain
 *        (scheduler.h:518-525). Proven by the live-frame counter returning to baseline after the drain.
 */
TEST_F(CoroutineSchedulerTests, DeferDestroyDrainsCompletedFrame) {
    auto &sched = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> done{false};
    auto done_ptr = &done;

    // A spawned root that awaits an inner task: it reaches final_suspend via symmetric transfer,
    // so its frame is reclaimed only through the defer_destroy / frames_to_destroy_ drain.
    coro_scheduler().spawn([done_ptr]() -> task<void> {
        auto inner = []() -> task<int> { co_return 7; };
        int v = co_await inner();
        EXPECT_EQ(v, 7);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deferred-destroy coroutine never completed";

    // After the drain that completed it, the deferred frame is freed and the counter is back to baseline.
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "defer_destroy drain leaked " << (after - baseline) << " frame(s)";
    EXPECT_EQ(sched.active_count(), 0u);
}

// =============================================================================
// TLS SCHEDULER FALLBACK (current() lazy-create on a bare thread)
// =============================================================================

/**
 * @test current() lazily creates a scheduler on a thread with no listener
 * @brief On a fresh worker thread that never created a listener, CoroutineScheduler::current() allocates
 *        the fallback scheduler on first access (scheduler.h:568-573) and current_ptr() then matches it.
 *
 * Runs on a dedicated thread so it cannot disturb the fixture's main-thread TLS scheduler. The fallback
 * is intentionally leaked until thread exit (documented), so we do not delete it here.
 */
TEST_F(CoroutineSchedulerTests, CurrentLazilyCreatesSchedulerOnBareThread) {
    std::atomic<void *> ptr_before{reinterpret_cast<void *>(0x1)};
    std::atomic<void *> from_current{nullptr};
    std::atomic<void *> from_ptr{nullptr};

    auto before_ptr = &ptr_before;
    auto cur_ptr    = &from_current;
    auto cptr_ptr   = &from_ptr;

    std::thread th([before_ptr, cur_ptr, cptr_ptr]() {
        // No init()/listener on this thread: current_ptr() is null before the first current() call.
        before_ptr->store(CoroutineScheduler::current_ptr());
        auto &sched = CoroutineScheduler::current(); // lazy-creates the fallback
        cur_ptr->store(&sched);
        cptr_ptr->store(CoroutineScheduler::current_ptr());
    });
    th.join();

    EXPECT_EQ(ptr_before.load(), nullptr) << "current_ptr() should be null before current() is called";
    EXPECT_NE(from_current.load(), nullptr) << "current() must lazily create a fallback scheduler";
    EXPECT_EQ(from_current.load(), from_ptr.load())
        << "current_ptr() must report the scheduler current() just created";
}

// =============================================================================
// STANDALONE-SCHEDULER TEARDOWN PATHS (~CoroutineScheduler drains)
//
// The fixture's TLS scheduler is owned by `listener::current` and is not destroyed
// mid-test, so the scheduler *destructor* drains (scheduler.h:153-198) are never hit
// by the cases above. These tests build a throwaway `CoroutineScheduler` we own
// outright, populate it deterministically through its public API, then let it die at
// end of scope so the destructor body runs while we can still assert no frame leaked.
//
// Routing contract: a spawned coroutine that reaches final_suspend with no
// continuation calls `defer_frame_destruction(h)`, which routes to
// `CoroutineScheduler::current_ptr()` — NOT to the frame's promise `scheduler_`. So to
// make a frame defer into our standalone scheduler we must install it as the TLS
// `current` for the duration we resume frames, then restore the fixture's scheduler.
// A tiny RAII guard does exactly that so even an assertion failure restores TLS.
// =============================================================================

namespace {

/// Install `s` as the thread's current scheduler for a scope; restore the previous
/// pointer on destruction so the fixture TearDown still sees a valid TLS scheduler.
struct ScopedCurrentScheduler {
    CoroutineScheduler *previous_;
    explicit ScopedCurrentScheduler(CoroutineScheduler &s) noexcept
        : previous_(CoroutineScheduler::current_ptr()) {
        CoroutineScheduler::set_current(&s);
    }
    ~ScopedCurrentScheduler() {
        CoroutineScheduler::set_current(previous_);
    }
};

// `spawn_tracked(task<void>&&)` takes a real task rvalue, NOT a Callable (only `spawn` has the
// Callable overload). We must therefore hand it a materialized task. Building that task from an
// immediately-invoked *capturing* lambda would dangle: a lambda-coroutine's frame holds only a
// pointer to the closure object, so the closure dying at the end of the full expression leaves the
// frame's captures dangling (the classic spawn(lambda()) footgun). The frames below are spawned
// detached and may outlive the call site, so we use plain coroutine FUNCTIONS that take their deps
// as by-value parameters — C++ copies parameters into the coroutine frame, so nothing dangles.

// Trivial completion: reaches final_suspend on the first resume, no side effect.
static task<void>
trivial_completion_coro() {
    co_return;
}

// Records that its body ran by storing true into the supplied flag (taken BY VALUE — copied into
// the frame). Used to prove a frame destroyed at initial_suspend never executes its body.
static task<void>
flag_setting_coro(std::atomic<bool> *flag) {
    flag->store(true);
    co_return;
}

} // namespace

/**
 * @test ~CoroutineScheduler destroys an owned, never-resumed ready handle
 * @brief Destructor ready-queue drain owned-frame branch (scheduler.h:164-168).
 *
 * spawn_tracked() puts a frame in BOTH the ready queue and owned_frames_ but leaves it
 * parked at initial_suspend (we never run_ready()). When the scheduler dies the ready
 * drain finds a handle that is `!done()` and owned, so it destroys it (line 167). The
 * frame never ran its body, so no user side effect fires and the live-frame counter
 * returns to baseline — proving the owned ready handle was freed exactly once.
 */
TEST_F(CoroutineSchedulerTests, DestructorDestroysOwnedReadyHandle) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> body_ran{false};
    auto              body_ptr = &body_ran;

    {
        CoroutineScheduler      standalone{qb::io::async::listener::current.loop()};
        ScopedCurrentScheduler  guard{standalone};

        // by-value-parameter coroutine: body_ptr is copied into the frame, no dangling closure.
        auto h = standalone.spawn_tracked(flag_setting_coro(body_ptr));
        ASSERT_TRUE(h);
        // Parked at initial_suspend, queued + owned, never resumed.
        EXPECT_EQ(standalone.pending_count(), 1u);
        EXPECT_FALSE(h.done());
        // standalone dies here: the ready-drain destroys the owned, not-done handle.
    }

    EXPECT_FALSE(body_ran.load()) << "the destroyed ready frame must not have run its body";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "destructor leaked the owned ready frame";

    // Restore a valid TLS scheduler for the fixture TearDown.
    CoroutineScheduler::set_current(&qb::io::async::listener::current.coro_scheduler());
}

/**
 * @test ~CoroutineScheduler drains frames_to_destroy_ at teardown
 * @brief Destructor deferred-destroy drain (scheduler.h:175-181) — the branch that frees
 *        a completed spawned frame that no final run_ready() reclaimed.
 *
 * We spawn_tracked a trivial `co_return` frame and resume it ONCE *outside* run_ready().
 * It reaches final_suspend and, because our standalone scheduler is the TLS current, hands
 * itself to `defer_frame_destruction` → it lands in frames_to_destroy_ AND stays in
 * owned_frames_ (no run_ready() runs to drain it). Destroying the scheduler then exercises
 * the teardown loop at 175-181, which erases it from owned_frames_ and frees the frame.
 */
TEST_F(CoroutineSchedulerTests, DestructorDrainsDeferredDestroyFrames) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    {
        CoroutineScheduler     standalone{qb::io::async::listener::current.loop()};
        ScopedCurrentScheduler guard{standalone};

        auto h = standalone.spawn_tracked(trivial_completion_coro());
        ASSERT_TRUE(h);

        // Drive the body to final_suspend WITHOUT run_ready() so the deferred frame is
        // never drained by run_ready — it must persist into the destructor.
        h.resume();
        ASSERT_TRUE(h.done()) << "frame should be parked at final_suspend after one resume";
        EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline)
            << "the completed-but-undrained frame must still be alive before teardown";
        // standalone dies here: the frames_to_destroy_ teardown loop frees it.
    }

    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "destructor failed to drain the deferred-destroy frame";

    CoroutineScheduler::set_current(&qb::io::async::listener::current.coro_scheduler());
}

/**
 * @test cancel_spawned() scrubs a handle out of frames_to_destroy_ before freeing it
 * @brief The defensive deferred-destroy scrub branch (scheduler.h:294-299, esp. the erase
 *        at :296) — a frame that already reached final_suspend (so it is owned AND queued
 *        for deferred destruction) must be removed from frames_to_destroy_ when
 *        cancel_spawned frees it, so no later drain double-frees it.
 *
 * Construction: spawn_tracked + one manual resume parks the frame at final_suspend with the
 * standalone scheduler as TLS current, so it is simultaneously in owned_frames_ and
 * frames_to_destroy_. cancel_spawned() then passes the ownership gate, finds it in
 * frames_to_destroy_ (the :296 erase), and destroys it exactly once.
 */
TEST_F(CoroutineSchedulerTests, CancelSpawnedScrubsDeferredDestroyEntry) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    CoroutineScheduler     standalone{qb::io::async::listener::current.loop()};
    ScopedCurrentScheduler guard{standalone};

    auto h = standalone.spawn_tracked(trivial_completion_coro());
    ASSERT_TRUE(h);
    h.resume(); // -> final_suspend -> defer_frame_destruction routes into standalone
    ASSERT_TRUE(h.done());
    EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "frame should be alive and queued for deferred destruction";

    // The owned + deferred frame: cancel_spawned passes the ownership gate, scrubs it from
    // frames_to_destroy_ (line 296), and frees it once.
    standalone.cancel_spawned(h);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "cancel_spawned failed to free the deferred frame";

    // A second cancel of the now-freed handle hits the ownership gate and is a safe no-op:
    // owned_frames_ no longer contains it, so it returns before any destroy.
    standalone.cancel_spawned(h);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
    EXPECT_EQ(standalone.active_count(), 0u);
}

/**
 * @test destroy_all_suspended() scrubs an owned root out of frames_to_destroy_
 * @brief The owned-root cascade's deferred-destroy scrub (scheduler.h:676-681) — an owned
 *        root that already reached final_suspend is BOTH in owned_frames_ and
 *        frames_to_destroy_. The cascade must erase it from frames_to_destroy_ (so the
 *        later deferred drain can never re-free it). At final_suspend the handle is `done()`,
 *        so the cascade's `!handle.done()` guard (line 683) correctly declines to destroy it
 *        — the frame is now orphaned of all scheduler bookkeeping and we reclaim it ourselves
 *        (a scheduler-owned spawn_tracked frame, no task<T> owner — the single legitimate
 *        manual destroy site).
 *
 * NOTE on active_count(): we resume the frame manually (h.resume()) instead of via run_ready()
 * to land it in frames_to_destroy_ WITHOUT draining it (run_ready() would free it on the same
 * tick). That manual path deliberately bypasses the ready-queue pop, so the frame is still
 * sitting in the scheduler's ready queue (done(), harmless) when destroy_all_suspended() runs.
 * destroy_all_suspended() scrubs owned_frames_ + frames_to_destroy_ + suspended_coroutines_ but
 * (by contract) NOT the ready queue — in real flows run_ready() already emptied it. So
 * active_count() reflects that one stale ready entry (1, not 0); a later run_ready() would pop it,
 * see done(), and neither resume nor re-destroy it. We assert the REAL contract: the scrub left
 * frames_to_destroy_ empty so our manual destroy below is the sole, safe destroyer (no double-free).
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedScrubsOwnedRootFromDeferredDestroy) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    CoroutineScheduler     standalone{qb::io::async::listener::current.loop()};
    ScopedCurrentScheduler guard{standalone};

    auto h = standalone.spawn_tracked(trivial_completion_coro());
    ASSERT_TRUE(h);
    h.resume(); // -> final_suspend: owned AND in frames_to_destroy_ (NOT drained — manual resume)
    ASSERT_TRUE(h.done());

    // The cascade snapshots owned_frames_, scrubs this addr out of frames_to_destroy_
    // (lines 676-681), then declines to destroy it because it is already done().
    standalone.destroy_all_suspended();
    // The owned-root cascade does not touch the ready queue, so the stale (done) ready entry left
    // by the manual resume survives — active_count() counts it. suspended_coroutines_ is empty.
    EXPECT_EQ(standalone.active_count(), 1u)
        << "manual resume left a done() entry in the ready queue; destroy_all_suspended() "
           "does not scrub it (only owned/suspended/deferred bookkeeping)";

    // Drain the stale ready entry BEFORE freeing the frame (the frame is still alive here). The
    // deferred-destroy scrub emptied frames_to_destroy_, so run_ready() must NOT free this frame:
    // it pops the entry, sees done(), and runs nothing — proving the scrub prevented a double-free.
    // (Draining first also avoids a later run_ready() calling done() on freed memory.)
    EXPECT_EQ(standalone.run_ready(), 0u) << "stale done() ready entry must not be resumed";
    EXPECT_EQ(standalone.active_count(), 0u) << "ready queue drained after run_ready()";

    // The frame is now removed from EVERY scheduler container but NOT freed (done-guard + the
    // run_ready() drain above declined to free it). It is a scheduler-owned (spawn_tracked) frame
    // with no task<T> owner, so we destroy it exactly once here — nothing else will (no double-free).
    EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline);
    h.destroy();
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "owned-root scrub path leaked the frame";
}

/**
 * @test destroy_all_suspended() sweeps a non-owned suspended frame
 * @brief The trailing non-owned suspended sweep (scheduler.h:693-702) — a frame that is
 *        registered as suspended on a watcher but is NOT owned by the scheduler (never
 *        spawned) is destroyed by the second pass, stopping its watcher via the awaiter
 *        destructor.
 *
 * Construction: build a plain `task<void>` that awaits a long `sleep` and resume it once so
 * its timer_awaiter parks it (calling register_suspended on the fixture's TLS scheduler).
 * The frame is therefore in suspended_coroutines_ but NOT owned_frames_ (it was never
 * spawned). We `detach()` the task so the task<T> object relinquishes ownership; then
 * destroy_all_suspended()'s owned-root pass is a no-op (owned_frames_ empty) and the trailing
 * sweep destroys the lone suspended frame — exactly once, with the timer stopped so it can
 * never fire afterwards.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedSweepsNonOwnedSuspendedFrame) {
    auto      &sched    = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> fired{false};
    auto              fired_ptr = &fired;

    // A non-spawned task parked on a long timer: register_suspended runs, but the frame is
    // never inserted into owned_frames_ (no spawn).
    auto parked = [fired_ptr]() -> task<void> {
        co_await sleep(5000ms);
        fired_ptr->store(true); // must NEVER fire — frame destroyed while parked
    }();

    auto h = parked.handle();
    ASSERT_TRUE(h);
    h.resume(); // run body to the sleep awaiter -> register_suspended (NOT owned)
    ASSERT_FALSE(h.done());
    EXPECT_EQ(sched.active_count(), 1u) << "the non-owned frame should be tracked as suspended";

    // Relinquish task<T> ownership so destroy_all_suspended is the single destroyer.
    auto raw = parked.detach();
    ASSERT_EQ(raw, h);

    // owned_frames_ is empty here (frame was never spawned), so only the trailing
    // non-owned suspended sweep (lines 693-702) destroys it.
    sched.destroy_all_suspended();
    EXPECT_EQ(sched.active_count(), 0u) << "non-owned suspended frame not swept";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline)
        << "destroy_all_suspended leaked the non-owned suspended frame";

    // The awaiter destructor stopped the timer; pumping the loop must not fire the body.
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(fired.load()) << "the destroyed frame's timer wrongly fired";
}

// =============================================================================
// FREE-FUNCTION ACCESSOR
// =============================================================================

/**
 * @test current_scheduler_ptr() mirrors CoroutineScheduler::current_ptr()
 * @brief The free helper used by awaiters to fetch the TLS scheduler (scheduler.h:783-786).
 *        It must return the exact same pointer as the static accessor — non-null while a
 *        listener scheduler is installed, and null after a reset.
 */
TEST_F(CoroutineSchedulerTests, CurrentSchedulerPtrFreeFunctionMatchesStatic) {
    // The fixture SetUp only calls async::init(), which is a no-op: the listener's
    // CoroutineScheduler is created lazily and installed as the TLS current pointer ONLY on the
    // first coro_scheduler() access (listener.h:636-640). So we must touch it here to install it
    // before asserting the accessors are non-null — otherwise current_scheduler_ptr() is
    // legitimately null (no scheduler exists yet).
    auto *installed = &qb::io::async::listener::current.coro_scheduler();

    // With the listener scheduler now installed, the free function and the static accessor agree
    // and are non-null, both pointing at the listener's scheduler.
    auto *via_static = CoroutineScheduler::current_ptr();
    auto *via_free   = current_scheduler_ptr();
    ASSERT_NE(via_free, nullptr);
    EXPECT_EQ(via_free, via_static);
    EXPECT_EQ(via_free, installed);

    // After a reset the free function reports null in lock-step with the static one.
    qb::io::async::listener::current.reset_coro_scheduler();
    EXPECT_EQ(current_scheduler_ptr(), nullptr);
    EXPECT_EQ(current_scheduler_ptr(), CoroutineScheduler::current_ptr());

    // Re-establish the scheduler so the fixture TearDown finds a consistent state.
    (void) qb::io::async::listener::current.coro_scheduler();
    EXPECT_EQ(current_scheduler_ptr(), &qb::io::async::listener::current.coro_scheduler());
}
