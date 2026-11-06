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
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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

    EXPECT_TRUE(pump_until([&] { return executed.load(std::memory_order_acquire); })) << "spawned coroutine never executed";
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

    EXPECT_TRUE(pump_until([&] { return completed.load(); })) << "spawned coroutine was cancelled when its originating closure was released";
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

/// The frame oracle of the move-assignment test below: a coroutine parameter lives in the frame, so
/// it is destroyed exactly when (and as many times as) the frame is. Moved-from copies do not count.
struct task_frame_witness {
    int *destroyed;
    explicit task_frame_witness(int *d) noexcept
        : destroyed(d) {}
    task_frame_witness(task_frame_witness &&other) noexcept
        : destroyed(std::exchange(other.destroyed, nullptr)) {}
    task_frame_witness &operator=(task_frame_witness &&) = delete;
    ~task_frame_witness() {
        if (destroyed)
            ++*destroyed;
    }
};

/**
 * @test Self move-assignment keeps the frame (Huly QB-360)
 * @brief `t = std::move(t)` released the task's own frame and then took the very handle it had just
 *        destroyed, so the task owned a dangling frame that its destructor destroyed a second time.
 *        It is a no-op now, for both specialisations; a distinct move-assignment still releases the
 *        assigned-over frame exactly once and empties the source.
 */
TEST_F(CoroutineSchedulerTests, TaskSelfMoveAssignmentKeepsItsFrame) {
    int destroyed = 0;
    {
        auto t = [](task_frame_witness) -> task<int> {
            co_return 7;
        }(task_frame_witness{&destroyed});
        auto &self = t; // an alias, so -Wself-move does not reject the statement under test
        t          = std::move(self);
        EXPECT_EQ(destroyed, 0) << "task<int>: a self move-assignment must not destroy the task's own frame";
        EXPECT_TRUE(static_cast<bool>(t)) << "task<int>: the task must still own its frame";
    }
    EXPECT_EQ(destroyed, 1) << "task<int>: the frame is destroyed once, by the task's destructor";

    destroyed = 0;
    {
        auto t = [](task_frame_witness) -> task<void> {
            co_return;
        }(task_frame_witness{&destroyed});
        auto &self = t;
        t          = std::move(self);
        EXPECT_EQ(destroyed, 0) << "task<void>: a self move-assignment must not destroy the task's own frame";
        EXPECT_TRUE(static_cast<bool>(t)) << "task<void>: the task must still own its frame";
    }
    EXPECT_EQ(destroyed, 1) << "task<void>: the frame is destroyed once, by the task's destructor";

    // Control: distinct tasks. The assigned-over frame goes at once, the source is emptied, and the
    // moved-in frame is destroyed once more at scope end -- two frames, two destructions.
    destroyed = 0;
    {
        auto make = [](task_frame_witness) -> task<int> {
            co_return 1;
        };
        auto a = make(task_frame_witness{&destroyed});
        auto b = make(task_frame_witness{&destroyed});
        a      = std::move(b);
        EXPECT_EQ(destroyed, 1) << "the assigned-over frame is released by the move-assignment";
        EXPECT_FALSE(static_cast<bool>(b)) << "the source is emptied";
        EXPECT_TRUE(static_cast<bool>(a));
    }
    EXPECT_EQ(destroyed, 2);
}

/**
 * @test co_await on a moved-from (empty) task fails loudly
 * @brief A task whose handle was moved out is empty; `co_await`ing it must throw std::logic_error
 *        from await_resume rather than dereference a null coroutine frame (the null-handle guard in
 *        task<T>::await_resume / task<void>::await_resume). Covers both specialisations.
 */
TEST_F(CoroutineSchedulerTests, CoAwaitMovedFromTaskThrowsLogicError) {
    std::atomic<bool> threw_int{false};
    std::atomic<bool> threw_void{false};
    std::atomic<bool> done{false};

    auto ti = &threw_int;
    auto tv = &threw_void;
    auto d  = &done;
    coro_scheduler().spawn([ti, tv, d]() -> task<void> {
        // task<int>: move the handle out, then co_await the empty husk → await_resume throws.
        auto t = []() -> task<int> {
            co_return 5;
        }();
        auto sink = std::move(t); // `sink` owns the never-run frame; `t` is now empty
        try {
            (void) co_await t;
        } catch (const std::logic_error &) {
            ti->store(true);
        }
        // task<void>: same contract.
        auto tvoid = []() -> task<void> {
            co_return;
        }();
        auto sinkv = std::move(tvoid);
        try {
            co_await tvoid;
        } catch (const std::logic_error &) {
            tv->store(true);
        }
        d->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "co_await empty-task coordinator never finished";
    EXPECT_TRUE(threw_int.load()) << "co_await on a moved-from task<int> must throw std::logic_error";
    EXPECT_TRUE(threw_void.load()) << "co_await on a moved-from task<void> must throw std::logic_error";
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

    // The coroutine parks on a channel the test opens, never on a timer: a 10 ms sleep against the 2 ms run below
    // left the "still parked" checks a ceiling an 8 ms stall breaks -- the ASan suite under a parallel ctest
    // completed it first (a ceiling, unlike this suite's order and elapsed-floor checks: see
    // coroutine_test_support.h).
    auto destroyed_ptr = &frame_destroyed;
    auto completed_ptr = &completed;
    auto gate          = std::make_shared<channel<int>>(1);
    coro_scheduler().spawn([destroyed_ptr, completed_ptr, gate]() -> task<void> {
        FrameMarker marker{destroyed_ptr};
        auto        opened = co_await gate->recv();
        completed_ptr->store(opened.has_value());
        co_return;
        // marker destroyed here, as part of frame teardown
    });

    // While parked the frame (hence the local) is still alive, however long the loop turns.
    qb::io::async::run_for(2ms);
    EXPECT_FALSE(completed.load());
    EXPECT_FALSE(frame_destroyed.load());

    ASSERT_TRUE(gate->try_send(1));
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

    auto                                        result_ptr = &result;
    const qb::io::test::coroutine_scope_cleanup cleanup;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        auto inner_fn = []() -> task<int> {
            co_return 42;
        };
        auto inner = inner_fn();
        inner.handle().resume();
        EXPECT_TRUE(inner.handle().done());
        int val = co_await inner;
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
    EXPECT_TRUE(pump_until([&] { return coro_scheduler().active_count() == 0u; })) << "scheduler never drained the throwing coroutine";
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

    auto inner_ptr  = &inner_started;
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
 * @brief A handle already in-flight is not queued a second time (schedule_resume / enqueue_for_later dedup branches).
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
    // The closure must outlive the task: the body only starts on a later run_ready()
    // (initial_suspend is suspend_always), so an immediately-invoked temporary would be dead.
    auto manual_fn = [stage_ptr]() -> task<void> {
        co_await std::suspend_always{}; // never resumes on its own
        stage_ptr->fetch_add(1000);
    };
    auto manual = manual_fn();
    auto h      = manual.handle();
    ASSERT_TRUE(h);

    // First schedule_resume queues it; the second hits the in_flight_ dedup and is a no-op.
    sched.schedule_resume(h);
    EXPECT_EQ(sched.pending_count(), 1u);
    sched.schedule_resume(h);
    EXPECT_EQ(sched.pending_count(), 1u) << "schedule_resume failed to dedup an in-flight handle";

    // enqueue_for_later for an already-in-flight handle also dedups.
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
 * @brief The early-return guards (schedule_resume / enqueue_for_later) — a null or completed handle is never queued.
 */
TEST_F(CoroutineSchedulerTests, ScheduleAndEnqueueRejectNullAndDone) {
    auto &sched = coro_scheduler();
    ASSERT_EQ(sched.pending_count(), 0u);

    // Null handles: both guards bail before touching the queue.
    sched.schedule_resume(std::coroutine_handle<>{});
    sched.enqueue_for_later(std::coroutine_handle<>{});
    EXPECT_EQ(sched.pending_count(), 0u);

    // A completed (done) handle: build a trivial task, run it to done, then verify it is refused.
    auto t = []() -> task<void> {
        co_return;
    }();
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
 * @brief forget(null) returns immediately; forget() of a handle the scheduler never
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
    auto t = []() -> task<void> {
        co_await std::suspend_always{};
    }();
    auto h = t.handle();
    sched.forget(h);
    EXPECT_EQ(sched.active_count(), 0u);
}

/**
 * @test register_suspended / unregister_suspended ignore a null handle
 * @brief Both null guards (register_suspended / unregister_suspended) — calling with an empty handle must not change the
 *        suspended count.
 */
TEST_F(CoroutineSchedulerTests, RegisterSuspendedNullHandleIgnored) {
    auto      &sched  = coro_scheduler();
    const auto before = sched.active_count();

    sched.register_suspended(std::coroutine_handle<>{});
    sched.unregister_suspended(std::coroutine_handle<>{});

    EXPECT_EQ(sched.active_count(), before) << "null handle wrongly mutated the suspended set";
}

/**
 * @test run_ready() refuses re-entrant invocation
 * @brief The run_ready re-entrancy guard returns 0 when run_ready() is called from inside a
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
 *        true (in_run_ready_ flag set by the ReentrancyGuard).
 */
TEST_F(CoroutineSchedulerTests, IsDrainingReadyReflectsRunReadyWindow) {
    auto &sched = coro_scheduler();
    EXPECT_FALSE(sched.is_draining_ready());

    std::atomic<bool> saw_draining{false};
    auto              saw_ptr = &saw_draining;
    coro_scheduler().spawn([saw_ptr]() -> task<void> {
        saw_ptr->store(coro_scheduler().is_draining_ready());
        co_return;
    });

    sched.run_ready();
    EXPECT_TRUE(saw_draining.load()) << "is_draining_ready() should be true inside a run_ready() drain";
    EXPECT_FALSE(sched.is_draining_ready()) << "draining flag must clear after run_ready() returns";
}

/**
 * @test A listener pass run from inside a coroutine leaves coroutines to the enclosing drain (Huly QB-253)
 * @brief Library code drains the loop from a coroutine body -- `Redis::await()`, `Transaction::await()`, and
 *        qbm-pgsql's `disconnect()` until 3.3 -- through `listener::current.run()`, which called `run_ready()`
 *        re-entrantly: an abort in a debug build (the guard's assert), a silent 0 in release. The nested
 *        run_ready() now returns 0 by definition: the pass runs the watchers and deferred callbacks and the
 *        enclosing drain resumes what it made ready, after the running coroutine. The debug abort is the defect.
 */
TEST_F(CoroutineSchedulerTests, NestedListenerPassLeavesCoroutinesToTheEnclosingDrain) {
    auto                    &sched = coro_scheduler();
    std::vector<std::string> order;
    auto                     order_ptr = &order;
    sched.spawn([order_ptr]() -> task<void> {
        defer([order_ptr] { order_ptr->push_back("deferred callback"); });
        coro_scheduler().spawn([order_ptr]() -> task<void> {
            order_ptr->push_back("sibling coroutine");
            co_return;
        });
        listener::current.run(EVRUN_NOWAIT); // what a library drain does from a coroutine body
        order_ptr->push_back("after the nested pass");
        co_return;
    });

    sched.run_ready();
    EXPECT_EQ(order, (std::vector<std::string>{"deferred callback", "after the nested pass", "sibling coroutine"}))
        << "the nested pass runs the deferred callback, resumes no coroutine, and the enclosing drain resumes the sibling";
}

// =============================================================================
// SPAWN_TRACKED / CANCEL_SPAWNED / DESTROY_ALL_SUSPENDED (helper lifecycle)
// =============================================================================

/**
 * @test spawn_tracked() returns an empty handle for an empty task
 * @brief The early-return when detach() yields no handle (spawn_tracked).
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
 *        timer, stopping its watcher and dropping it from active_count without
 *        waiting for the timer. Drives the cancel_spawned ownership-gate + bookkeeping-scrub path.
 */
TEST_F(CoroutineSchedulerTests, SpawnTrackedThenCancelReclaimsHelper) {
    auto      &sched    = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> ran_past_sleep{false};
    auto              ran_ptr = &ran_past_sleep;

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
 * @brief The cancel_spawned null guard and ownership gate: a handle the
 *        scheduler never owned is never destroyed.
 */
TEST_F(CoroutineSchedulerTests, CancelSpawnedNullAndNonOwnedNoOp) {
    auto &sched = coro_scheduler();

    sched.cancel_spawned(std::coroutine_handle<>{}); // null guard
    EXPECT_EQ(sched.active_count(), 0u);

    // A live frame the scheduler does not own: cancel_spawned must NOT free it (ownership gate).
    auto t = []() -> task<void> {
        co_await std::suspend_always{};
    }();
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
 * @brief Drives both passes of destroy_all_suspended: the owned-root cascade (a
 *        spawn_tracked helper parked on a non-cancellable sleep) and the trailing non-owned suspended
 *        sweep. After it runs, active_count() is 0 and no frame leaked, with the loop still valid.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedTearsDownMixedFrames) {
    auto      &sched    = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<int> fired{0};
    auto             fired_ptr = &fired;

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
 * @brief A spawned coroutine completing via symmetric transfer hands its frame to defer_destroy.
 *        run_ready() then frees it on its deferred-destroy drain. The live-frame counter
 *        returning to baseline after that drain verifies reclamation.
 */
TEST_F(CoroutineSchedulerTests, DeferDestroyDrainsCompletedFrame) {
    auto      &sched    = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> done{false};
    auto              done_ptr = &done;

    // A spawned root that awaits an inner task: it reaches final_suspend via symmetric transfer,
    // so its frame is reclaimed only through the defer_destroy / frames_to_destroy_ drain.
    coro_scheduler().spawn([done_ptr]() -> task<void> {
        auto inner = []() -> task<int> {
            co_return 7;
        };
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
 * @test current() lazily binds a scheduler on a thread with no listener
 * @brief On a fresh worker thread that never created a listener, CoroutineScheduler::current() binds the
 *        thread's `listener::current` scheduler on first access (creating the listener) through
 *        `detail::bind_thread_scheduler()`, and current_ptr() then matches it.
 *
 * Runs on a dedicated thread so it cannot disturb the fixture's main-thread TLS scheduler. The listener
 * and its scheduler are thread_local and die with the thread.
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
        auto &sched = CoroutineScheduler::current(); // binds the thread's loop scheduler
        cur_ptr->store(&sched);
        cptr_ptr->store(CoroutineScheduler::current_ptr());
    });
    th.join();

    EXPECT_EQ(ptr_before.load(), nullptr) << "current_ptr() should be null before current() is called";
    EXPECT_NE(from_current.load(), nullptr) << "current() must lazily bind a scheduler";
    EXPECT_EQ(from_current.load(), from_ptr.load()) << "current_ptr() must report the scheduler current() just created";
}

/**
 * @test current() binds again a loop scheduler that exists but was unbound
 * @brief After `set_current(nullptr)` the thread's `listener::current` scheduler is still alive, and
 *        `coro_scheduler()` binds only a scheduler it creates. `current()` must return that scheduler AND
 *        bind it again -- otherwise current_ptr() stays null and every later call on the thread takes the
 *        out-of-line cold path.
 *
 * Runs on a dedicated thread so it cannot disturb the fixture's main-thread TLS scheduler.
 */
TEST_F(CoroutineSchedulerTests, CurrentBindsAnUnboundLoopScheduler) {
    std::atomic<void *> loop{nullptr};
    std::atomic<void *> from_current{nullptr};
    std::atomic<void *> from_ptr{nullptr};

    std::thread th([&loop, &from_current, &from_ptr]() {
        loop.store(&qb::io::async::listener::current.coro_scheduler());
        CoroutineScheduler::set_current(nullptr);
        from_current.store(&CoroutineScheduler::current());
        from_ptr.store(CoroutineScheduler::current_ptr());
    });
    th.join();

    ASSERT_NE(loop.load(), nullptr);
    EXPECT_EQ(from_current.load(), loop.load()) << "current() must return the loop's scheduler";
    EXPECT_EQ(from_ptr.load(), loop.load()) << "current() must bind the loop's scheduler again, not only return it";
}

// =============================================================================
// STANDALONE-SCHEDULER TEARDOWN PATHS (~CoroutineScheduler drains)
//
// The fixture's TLS scheduler is owned by `listener::current` and is not destroyed
// mid-test, so the scheduler *destructor* drains (drain_for_teardown) are never hit
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
 * @brief Destructor ready-queue drain owned-frame branch (drain_for_teardown).
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
        CoroutineScheduler     standalone{};
        ScopedCurrentScheduler guard{standalone};

        // by-value-parameter coroutine: body_ptr is copied into the frame, no dangling closure.
        auto h = standalone.spawn_tracked(flag_setting_coro(body_ptr));
        ASSERT_TRUE(h);
        // Parked at initial_suspend, queued + owned, never resumed.
        EXPECT_EQ(standalone.pending_count(), 1u);
        EXPECT_FALSE(h.done());
        // standalone dies here: the ready-drain destroys the owned, not-done handle.
    }

    EXPECT_FALSE(body_ran.load()) << "the destroyed ready frame must not have run its body";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "destructor leaked the owned ready frame";

    // Restore a valid TLS scheduler for the fixture TearDown.
    CoroutineScheduler::set_current(&qb::io::async::listener::current.coro_scheduler());
}

/**
 * @test ~CoroutineScheduler drains frames_to_destroy_ at teardown
 * @brief Destructor deferred-destroy drain (drain_for_teardown) — the branch that frees
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
        CoroutineScheduler     standalone{};
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

    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "destructor failed to drain the deferred-destroy frame";

    CoroutineScheduler::set_current(&qb::io::async::listener::current.coro_scheduler());
}

/**
 * @test cancel_spawned() scrubs a handle out of frames_to_destroy_ before freeing it
 * @brief The defensive deferred-destroy scrub in cancel_spawned: a frame already at
 *        final_suspend (so it is owned AND queued
 *        for deferred destruction) must be removed from frames_to_destroy_ when
 *        cancel_spawned frees it, so no later drain double-frees it.
 *
 * Construction: spawn_tracked + one manual resume parks the frame at final_suspend with the
 * standalone scheduler as TLS current, so it is simultaneously in owned_frames_ and
 * frames_to_destroy_. cancel_spawned() then passes the ownership gate, finds it in
 * frames_to_destroy_, and destroys it exactly once.
 */
TEST_F(CoroutineSchedulerTests, CancelSpawnedScrubsDeferredDestroyEntry) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    CoroutineScheduler     standalone{};
    ScopedCurrentScheduler guard{standalone};

    auto h = standalone.spawn_tracked(trivial_completion_coro());
    ASSERT_TRUE(h);
    h.resume(); // -> final_suspend -> defer_frame_destruction routes into standalone
    ASSERT_TRUE(h.done());
    EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline) << "frame should be alive and queued for deferred destruction";

    // The owned + deferred frame: cancel_spawned passes the ownership gate, scrubs it from
    // frames_to_destroy_ (line 296), and frees it once.
    standalone.cancel_spawned(h);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "cancel_spawned failed to free the deferred frame";

    // A second cancel of the now-freed handle hits the ownership gate and is a safe no-op:
    // owned_frames_ no longer contains it, so it returns before any destroy.
    standalone.cancel_spawned(h);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
    EXPECT_EQ(standalone.active_count(), 0u);
}

/**
 * @test destroy_all_suspended() frees an owned root it scrubs out of frames_to_destroy_
 * @brief The owned-root cascade's deferred-destroy scrub — an owned root that already reached
 *        final_suspend is BOTH in owned_frames_ and frames_to_destroy_. The cascade erases it
 *        from frames_to_destroy_ (so the deferred drain can never re-free it) and, because the
 *        scheduler OWNS that frame and nothing else will ever free it, destroys it right there:
 *        a `done()` root is exactly what a completed spawned coroutine looks like, and until
 *        3.2 the cascade's `!handle.done()` guard declined it — a frame leaked per spawned
 *        coroutine that completed between the last run_ready() drain and the teardown. That
 *        window was empty while a spawned coroutine could only complete inside run_ready();
 *        it is not any more: an `ask` reply resumes its frame inline from the handler that
 *        routed it, so a coroutine reaches final_suspend inside a core pass, and its actor can
 *        die in the same pass (Huly QB-185; the ask round-trip suite measured the leak under
 *        ASan first).
 *
 * NOTE on the ready queue: we resume the frame manually (h.resume()) instead of via run_ready()
 * to land it in frames_to_destroy_ WITHOUT draining it (run_ready() would free it on the same
 * tick). That manual path bypasses the ready-queue pop, so a stale (done) ready entry for the
 * frame survives into destroy_all_suspended(), which must scrub it too: a later run_ready()
 * would otherwise call done() on freed memory. The contract pinned here is the whole of it —
 * after the cascade the frame is freed, no scheduler container names it, and a run_ready()
 * finds nothing to do.
 */
/**
 * @test has_work() sees a deferred frame and run_ready() frees it without resuming anything
 * @brief A spawned coroutine that completes OUTSIDE run_ready() — an `ask` reply resumes it
 *        inline from the handler that routed the reply (QB-185) — leaves its frame in
 *        frames_to_destroy_ with nothing in the ready queue. `has_ready()` is false, so a
 *        listener gating its `run()` on it would never drain the list and the frame would
 *        wait for the teardown; `has_work()` is what `listener::has_work()` asks instead,
 *        and it is true until a `run_ready()` turn — which resumes nothing — has freed it.
 */
TEST_F(CoroutineSchedulerTests, HasWorkSeesADeferredFrameAndRunReadyFreesIt) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    CoroutineScheduler     standalone{};
    ScopedCurrentScheduler guard{standalone};
    EXPECT_FALSE(standalone.has_work()) << "a fresh scheduler has nothing to do";

    auto h = standalone.spawn_tracked(trivial_completion_coro());
    ASSERT_TRUE(h);
    EXPECT_TRUE(standalone.has_ready() && standalone.has_work()) << "a spawned frame is ready, hence work";
    EXPECT_EQ(standalone.run_ready(), 1u); // pops it, resumes it: it completes and defers its own destruction...
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "...and the same turn's drain frees it";
    EXPECT_FALSE(standalone.has_work());

    // Consume the initial ready entry, park, then finish outside the scheduler drain.
    auto pause_once = []() -> task<void> {
        co_await std::suspend_always{};
    };
    h = standalone.spawn_tracked(pause_once());
    ASSERT_TRUE(h);
    EXPECT_EQ(standalone.run_ready(), 1u);
    ASSERT_FALSE(standalone.has_ready());
    EXPECT_FALSE(h.done());
    h.resume();
    ASSERT_TRUE(h.done());
    EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline) << "deferred, not freed";
    EXPECT_FALSE(standalone.has_ready());
    EXPECT_TRUE(standalone.has_work()) << "a frame to free is work even with nothing to resume";
    EXPECT_EQ(standalone.run_ready(), 0u) << "an empty ready queue still drains deferred destruction";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "the turn's drain freed the deferred frame";
    EXPECT_FALSE(standalone.has_work());
    EXPECT_EQ(standalone.active_count(), 0u);
}

TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedScrubsOwnedRootFromDeferredDestroy) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    CoroutineScheduler     standalone{};
    ScopedCurrentScheduler guard{standalone};

    auto h = standalone.spawn_tracked(trivial_completion_coro());
    ASSERT_TRUE(h);
    h.resume(); // -> final_suspend: owned AND in frames_to_destroy_ (NOT drained — manual resume)
    ASSERT_TRUE(h.done());
    EXPECT_GT(detail::CoroutineFrameAllocator::live_frames, baseline) << "the completed frame is still allocated (deferred)";

    // The cascade snapshots owned_frames_, scrubs this addr out of frames_to_destroy_ AND out of
    // the ready queue, then destroys it: the scheduler owns it and nobody else can.
    standalone.destroy_all_suspended();
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "owned-root scrub path leaked the frame";
    EXPECT_EQ(standalone.active_count(), 0u) << "the stale done() ready entry must go with the frame it names";

    // Nothing is left for a drain to touch — in particular no done() call on freed memory.
    EXPECT_EQ(standalone.run_ready(), 0u) << "nothing ready after the cascade";
    EXPECT_EQ(standalone.active_count(), 0u);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test destroy_all_suspended() leaves a frame it does not own to the task that owns it
 * @brief A frame registered as suspended on a watcher but NOT owned by the scheduler (never
 *        spawned) stays parked and registered: its task destroys it, once, and the awaiter's
 *        destructor stops the watcher then (Huly QB-1220).
 *
 * Construction: build a plain `task<void>` that awaits a long `sleep` and resume it once so
 * its timer_awaiter parks it (calling register_suspended on the fixture's TLS scheduler).
 * The frame is therefore in suspended_coroutines_ but NOT owned_frames_ (it was never
 * spawned). destroy_all_suspended() used to destroy it in a trailing sweep, and the task --
 * which still owns it -- destroyed it a second time; with `detach()` the test made the sweep
 * the single destroyer, which no owner can rely on (it cannot tell a frame a live task holds
 * from one nobody does). Now the owned-root pass is a no-op (owned_frames_ empty) and the
 * frame is the task's, and the scheduler forgets it.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedLeavesANonOwnedFrameToItsTask) {
    auto      &sched    = coro_scheduler();
    const long baseline = detail::CoroutineFrameAllocator::live_frames;

    std::atomic<bool> fired{false};
    auto              fired_ptr = &fired;

    // A non-spawned task parked on a long timer: register_suspended runs, but the frame is
    // never inserted into owned_frames_ (no spawn).
    auto parked_fn = [fired_ptr]() -> task<void> {
        co_await sleep(5000ms);
        fired_ptr->store(true); // must NEVER fire — frame destroyed while parked
    };
    {
        auto parked = parked_fn();
        auto h      = parked.handle();
        ASSERT_TRUE(h);
        h.resume(); // run body to the sleep awaiter -> register_suspended (NOT owned)
        ASSERT_FALSE(h.done());
        EXPECT_EQ(sched.active_count(), 1u) << "the non-owned frame should be tracked as suspended";

        sched.destroy_all_suspended();
        EXPECT_FALSE(h.done());
        EXPECT_EQ(sched.active_count(), 0u) << "the scheduler forgets a frame it does not own";
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 1) << "destroy_all_suspended destroyed a frame a task owns";
    } // the task destroys its frame; the awaiter's destructor stops the timer and leaves the suspended set
    EXPECT_EQ(sched.active_count(), 0u);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "the frame was destroyed twice, or never";

    // The awaiter destructor stopped the timer; pumping the loop must not fire the body.
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(fired.load()) << "the destroyed frame's timer wrongly fired";
}

// =============================================================================
// FREE-FUNCTION ACCESSOR
// =============================================================================

/**
 * @test current_scheduler_ptr() mirrors CoroutineScheduler::current_ptr()
 * @brief The free helper used by awaiters to fetch the TLS scheduler (scheduler.h:1166-1169).
 *        It must return the exact same pointer as the static accessor — non-null while a
 *        listener scheduler is installed, and null after a reset.
 */
TEST_F(CoroutineSchedulerTests, CurrentSchedulerPtrFreeFunctionMatchesStatic) {
    // The fixture SetUp only calls async::init(), which is a no-op: the listener's
    // CoroutineScheduler is created lazily and installed as the TLS current pointer ONLY on the
    // first coro_scheduler() access (listener::coro_scheduler). So we must touch it here to install it
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

// =============================================================================
// A CHAIN DRIVEN BY HAND, KEPT ACROSS reset_coro_scheduler() (Huly QB-1220)
// =============================================================================
// A task resumed by hand -- never spawned -- that parks on a wait the scheduler tracks (a timer, a channel's timed
// wait) is owned by its task<T>, not by the scheduler. destroy_all_suspended() destroyed it anyway, so a task kept
// across reset_coro_scheduler() destroyed its frame a second time; and its awaiter, which outlives that scheduler,
// reached the destroyed scheduler at its resume and at its destruction. The frames come from the coroutine frame pool,
// which AddressSanitizer does not see: the frame count is the witness of the double destruction, and the destroyed
// scheduler, an ordinary heap object, is what AddressSanitizer reports.

/**
 * @test A sleep driven by hand, kept across the reset, is destroyed once -- by its task -- and its timer never fires.
 */
TEST_F(CoroutineSchedulerTests, ResetLeavesAHandDrivenSleepToTheTaskThatOwnsIt) {
    const long        baseline  = detail::CoroutineFrameAllocator::live_frames;
    const std::size_t abandoned = abandoned_coroutine_frames_total();
    std::atomic<bool> resumed{false};
    auto const        body = [&resumed]() -> task<void> {
        co_await sleep(5000ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume(); // runs to the sleep: registered with the listener's scheduler, never spawned
        ASSERT_EQ(coro_scheduler().active_count(), 1u);
        qb::io::async::listener::current.reset_coro_scheduler();
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 1) << "the reset destroyed a frame its task owns";
        EXPECT_EQ(abandoned_coroutine_frames_total(), abandoned) << "a frame left to its task is not an abandoned one";
        (void) coro_scheduler(); // the scheduler of what follows
    } // `parked` destroys its frame, and the awaiter in it must not reach the scheduler the reset destroyed
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "the frame was destroyed twice, or never";
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(resumed.load()) << "the destroyed frame's timer fired";
}

/**
 * @test A sleep driven by hand, kept across the reset, still wakes -- in the scheduler that is current when its timer
 *       fires -- and completes.
 */
TEST_F(CoroutineSchedulerTests, AHandDrivenSleepKeptAcrossTheResetStillWakes) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> resumed{false};
    auto const        body = [&resumed]() -> task<void> {
        co_await sleep(5ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume();
        qb::io::async::listener::current.reset_coro_scheduler();
        (void) coro_scheduler();
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the timer of a frame kept across the reset never woke it";
        EXPECT_TRUE(parked.handle().done());
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A sleep driven by hand, kept across the reset, wakes even when no scheduler is bound when its timer fires: the
 *       resume binds the loop's scheduler, never the one the awaiter registered with, which the reset destroyed.
 */
TEST_F(CoroutineSchedulerTests, AHandDrivenSleepKeptAcrossTheResetWakesWithNoSchedulerBound) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> resumed{false};
    auto const        body = [&resumed]() -> task<void> {
        co_await sleep(5ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume();
        qb::io::async::listener::current.reset_coro_scheduler();
        ASSERT_EQ(CoroutineScheduler::current_ptr(), nullptr); // nothing bound until the timer fires
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the timer of a frame kept across the reset never woke it";
        EXPECT_TRUE(parked.handle().done());
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A channel's timed receive driven by hand, kept across the reset, is destroyed once -- by its task.
 * @brief recv_for() is a coroutine of its own: the reset destroyed that inner frame, and the task's frame destroyed it
 *        again through the inner task it awaits.
 */
TEST_F(CoroutineSchedulerTests, ResetLeavesAHandDrivenRecvForToTheTaskThatOwnsIt) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>       ch;
    std::optional<int> received;
    std::atomic<bool>  resumed{false};
    auto const         body = [&]() -> task<void> {
        received = co_await ch.recv_for(5000ms);
        resumed  = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume(); // parks in recv_for: its watchdog armed, the inner frame registered
        ASSERT_EQ(coro_scheduler().active_count(), 1u);
        qb::io::async::listener::current.reset_coro_scheduler();
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 2) << "the reset destroyed a frame a task owns";
        (void) coro_scheduler();
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "a frame was destroyed twice, or never";
    EXPECT_EQ(ch.waiter_census().selects, 0u) << "the destroyed receive is still linked to the channel";
    EXPECT_FALSE(resumed.load());
}

/**
 * @test A channel's timed receive driven by hand, kept across the reset, still receives -- in the next scheduler.
 */
TEST_F(CoroutineSchedulerTests, AHandDrivenRecvForKeptAcrossTheResetStillReceives) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>       ch;
    std::optional<int> received;
    std::atomic<bool>  resumed{false};
    auto const         body = [&]() -> task<void> {
        received = co_await ch.recv_for(5000ms);
        resumed  = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume();
        qb::io::async::listener::current.reset_coro_scheduler();
        (void) coro_scheduler();
        EXPECT_TRUE(ch.try_send(7));
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the receive kept across the reset never completed";
        EXPECT_EQ(received, std::optional<int>{7});
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A channel's timed send driven by hand, kept across the reset, is destroyed once -- by its task -- and leaves
 *       the channel as it found it.
 */
TEST_F(CoroutineSchedulerTests, ResetLeavesAHandDrivenSendForToTheTaskThatOwnsIt) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>      ch; // rendezvous: the send parks until a receiver comes
    std::atomic<bool> resumed{false};
    auto const        body = [&]() -> task<void> {
        (void) co_await ch.send_for(7, 5000ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume(); // parks in send_for: its watchdog armed, the inner frame registered
        ASSERT_EQ(coro_scheduler().active_count(), 1u);
        qb::io::async::listener::current.reset_coro_scheduler();
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 2) << "the reset destroyed a frame a task owns";
        (void) coro_scheduler();
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "a frame was destroyed twice, or never";
    EXPECT_EQ(ch.waiter_census().senders, 0u) << "the destroyed send is still linked to the channel";
    EXPECT_FALSE(resumed.load());
}

/**
 * @test A channel's timed send driven by hand, kept across the reset, still delivers -- to a receiver of the next
 *       scheduler.
 */
TEST_F(CoroutineSchedulerTests, AHandDrivenSendForKeptAcrossTheResetStillDelivers) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>      ch;
    bool              delivered = false;
    std::atomic<bool> resumed{false};
    int               got  = 0;
    auto const        body = [&]() -> task<void> {
        delivered = co_await ch.send_for(7, 5000ms);
        resumed   = true;
    };
    {
        task<void> parked = body();
        (void) coro_scheduler();
        parked.handle().resume();
        qb::io::async::listener::current.reset_coro_scheduler();
        coro_scheduler().spawn([&]() -> task<void> { got = (co_await ch.recv()).value_or(-1); });
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the send kept across the reset never completed";
        EXPECT_TRUE(delivered);
        EXPECT_EQ(got, 7);
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

#if !defined(_WIN32)
/**
 * @test A socket wait driven by hand, kept across the reset, still wakes when its descriptor becomes readable, and is
 *       destroyed once -- by its task -- when it is kept parked instead.
 */
TEST_F(CoroutineSchedulerTests, AHandDrivenSocketWaitKeptAcrossTheResetBelongsToItsTask) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> resumed{false};
    auto const        body = [&, read_fd = fds[0]]() -> task<void> {
        co_await wait_readable(read_fd);
        resumed = true;
    };
    {
        task<void> woken  = body();
        task<void> parked = body();
        (void) coro_scheduler();
        woken.handle().resume();
        parked.handle().resume();
        ASSERT_EQ(coro_scheduler().active_count(), 2u);
        qb::io::async::listener::current.reset_coro_scheduler();
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 2) << "the reset destroyed a frame a task owns";
        (void) coro_scheduler();
        parked = task<void>{}; // destroyed by its task, still parked: its watcher stopped, the scheduler untouched
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 1);
        char const byte = 'x';
        ASSERT_EQ(::write(fds[1], &byte, 1), 1);
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the socket wait kept across the reset never woke";
        EXPECT_TRUE(woken.handle().done());
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
    ::close(fds[0]);
    ::close(fds[1]);
}
#endif

namespace scheduler_lifecycle_test {
/// Names `s` the thread's current scheduler for a scope, whatever ends it (an ASSERT's early return included).
struct scoped_current {
    CoroutineScheduler *was;
    explicit scoped_current(CoroutineScheduler *s) noexcept
        : was(CoroutineScheduler::current_ptr()) {
        CoroutineScheduler::set_current(s);
    }
    ~scoped_current() {
        CoroutineScheduler::set_current(was);
    }
};
} // namespace scheduler_lifecycle_test

/**
 * @test destroy_all_suspended() on a scheduler that is not the thread's current one scrubs its OWN queues.
 * @brief An inner frame whose timer fired and queued it in this scheduler, destroyed through its owned root while
 *        another scheduler is current, must leave this scheduler's ready queue -- a later drain would otherwise read
 *        the freed frame. Its awaiter reaches the scheduler it registered with while that one lives (Huly QB-1220);
 *        the base did, through its cached pointer, and the first design of QB-1220, which went through the current one,
 *        did not.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedScrubsItsOwnQueuesWhenAnotherSchedulerIsCurrent) {
    auto *const                                             listener_scheduler = &coro_scheduler();
    const long                                              baseline           = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler                                      standalone;
    std::optional<scheduler_lifecycle_test::scoped_current> bound{std::in_place, &standalone};
    auto const                                              inner = []() -> task<void> {
        co_await sleep(1ms);
    };
    standalone.spawn([&inner]() -> task<void> { co_await inner(); });
    standalone.run_ready(); // the root awaits the inner task, which parks on its timer, registered with `standalone`
    for (int i = 0; i < 2000 && standalone.pending_count() == 0; ++i)
        qb::io::async::listener::current.run_once_for(1ms); // the timer fires: the inner frame is queued in `standalone`
    ASSERT_EQ(standalone.pending_count(), 1u) << "the inner frame's timer never queued it";
    EXPECT_EQ(standalone.active_count(), 1u) << "a fired timer is counted once: queued, no longer suspended";
    bound.reset();                      // the listener's scheduler current again
    standalone.destroy_all_suspended(); // the root, then the inner frame through it
    EXPECT_EQ(standalone.pending_count(), 0u) << "the destroyed inner frame is still queued in the scheduler that destroyed it";
    EXPECT_EQ(CoroutineScheduler::current_ptr(), listener_scheduler) << "the current scheduler was not restored";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A scheduler that runs frames while ANOTHER one is current, then destroys them, scrubs the scheduler they
 *       registered with -- the current one -- not itself.
 * @brief An awaiter registers with the scheduler current when it suspends; `run_ready()` does not make its own scheduler
 *        current. The destroyer reaches its own bookkeeping only for the frames that registered with it (Huly
 *        QB-1220: a design that made the destroyer current while it destroyed left this registration behind).
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedScrubsTheSchedulerItsFramesRegisteredWith) {
    auto             &listener_scheduler = coro_scheduler();
    const std::size_t active             = listener_scheduler.active_count();
    const long        baseline           = detail::CoroutineFrameAllocator::live_frames;
    {
        CoroutineScheduler standalone; // never current: what it runs suspends under the listener's scheduler
        auto const         inner = []() -> task<void> {
            co_await sleep(5000ms);
        };
        standalone.spawn([&inner]() -> task<void> { co_await inner(); });
        standalone.run_ready();
        ASSERT_EQ(listener_scheduler.active_count(), active + 1) << "the inner frame registers with the current scheduler";
        standalone.destroy_all_suspended(); // its root, and the inner frame through it
        EXPECT_EQ(listener_scheduler.active_count(), active) << "the destroyed frame is still registered where it registered";
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test recycle_coro_scheduler() -- a VirtualCore's teardown -- leaves a sleep driven by hand to the task that owns it:
 *       destroyed once, not reported, and still woken by its timer.
 */
TEST_F(CoroutineSchedulerTests, RecycleLeavesAHandDrivenSleepToTheTaskThatOwnsIt) {
    const long        baseline  = detail::CoroutineFrameAllocator::live_frames;
    const std::size_t abandoned = abandoned_coroutine_frames_total();
    std::atomic<bool> resumed{false};
    auto const        body = [&resumed]() -> task<void> {
        co_await sleep(5ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        auto      &sched  = coro_scheduler();
        parked.handle().resume();
        ASSERT_EQ(sched.active_count(), 1u);
        qb::io::async::listener::current.recycle_coro_scheduler();
        EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline + 1) << "the recycle destroyed a frame its task owns";
        EXPECT_EQ(sched.active_count(), 0u) << "the scheduler forgets a frame it does not own";
        EXPECT_EQ(abandoned_coroutine_frames_total(), abandoned) << "a frame left to its task is not an abandoned one";
        EXPECT_TRUE(pump_until([&] { return resumed.load(); })) << "the sleep kept across the recycle never woke";
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A frame a task owns, queued in a scheduler by a wait list (no watcher, no cached scheduler), leaves that
 *       scheduler's queues when the scheduler destroys the chain while ANOTHER one is current.
 * @brief An untimed channel receive is woken by a send: schedule_via_current() queues it in the scheduler then current.
 *        Its frame is a task the spawned root awaits, so ~task scrubs it -- from the current scheduler only, until
 *        Huly QB-1220: the destroying scheduler kept the freed frame queued, for its next drain to read.
 */
TEST_F(CoroutineSchedulerTests, DestroyAllSuspendedScrubsAWokenWaitListFrameFromItsOwnQueue) {
    const long                                              baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>                                            ch;
    CoroutineScheduler                                      standalone;
    std::optional<scheduler_lifecycle_test::scoped_current> bound{std::in_place, &standalone};
    auto const                                              inner = [&ch]() -> task<void> {
        (void) co_await ch.recv();
    };
    standalone.spawn([&inner]() -> task<void> { co_await inner(); });
    standalone.run_ready();      // the root awaits the inner task, parked on the channel
    ASSERT_TRUE(ch.try_send(1)); // wakes it: queued in `standalone`, the current scheduler
    ASSERT_EQ(standalone.pending_count(), 1u);
    bound.reset();
    standalone.destroy_all_suspended(); // the root, then the inner task's frame through its ~task
    EXPECT_EQ(standalone.pending_count(), 0u) << "the destroyed frame is still queued in the scheduler that destroyed it";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A frame destroyed through its raw handle while its scheduler destroys, that scheduler not being current,
 *       leaves that scheduler's queues: its awaiter reaches the scheduler it registered with while that one lives.
 */
TEST_F(CoroutineSchedulerTests, AFrameDestroyedByHandleDuringItsSchedulersDestroyLeavesItsQueues) {
    const long                                              baseline = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler                                      standalone;
    std::optional<scheduler_lifecycle_test::scoped_current> bound{std::in_place, &standalone};
    auto const                                              naps = []() -> task<void> {
        co_await sleep(1ms);
    };
    std::coroutine_handle<> raw = naps().detach(); // a frame its holder destroys by hand
    raw.resume();                                  // parks on its timer, registered with `standalone`
    struct destroys_on_exit {
        std::coroutine_handle<> h;
        ~destroys_on_exit() {
            h.destroy();
        }
    };
    standalone.spawn([raw]() -> task<void> {
        destroys_on_exit const holder{raw};
        co_await sleep(5000ms); // the spawned root that holds it, parked
    });
    standalone.run_ready();
    for (int i = 0; i < 2000 && standalone.pending_count() == 0; ++i)
        qb::io::async::listener::current.run_once_for(1ms); // the 1 ms timer fires: the raw frame queued in `standalone`
    ASSERT_EQ(standalone.pending_count(), 1u);
    bound.reset();
    standalone.destroy_all_suspended(); // the root, whose holder destroys the raw frame by its handle
    EXPECT_EQ(standalone.pending_count(), 0u) << "the frame destroyed by its handle is still queued in its scheduler";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test Destroying a listener that has no scheduler leaves the thread's current scheduler bound.
 * @brief ~listener runs reset_coro_scheduler(), which cleared the thread's current scheduler whatever it was: a
 *        temporary listener unbound the thread's own (Huly QB-1220), and every awaiter resolving "the current one"
 *        found none.
 */
TEST_F(CoroutineSchedulerTests, ATemporaryListenerLeavesTheThreadsSchedulerBound) {
    auto *const sched = &coro_scheduler();
    {
        qb::io::async::listener temporary;
    }
    EXPECT_EQ(CoroutineScheduler::current_ptr(), sched) << "destroying another listener unbound this thread's scheduler";
}

/**
 * @test A frame registered with a scheduler that is alive but not current -- nothing current at all -- is scrubbed
 *       from THAT scheduler when it is destroyed: the awaiter reaches the scheduler it registered with while it lives.
 */
TEST_F(CoroutineSchedulerTests, AFrameRegisteredWithALiveSchedulerIsScrubbedThereWhenNoneIsCurrent) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler standalone; // alive throughout; the listener's loop pumps its watchers, never its queue
    std::atomic<bool>  resumed{false};
    auto const         body = [&resumed]() -> task<void> {
        co_await sleep(1ms);
        resumed = true;
    };
    {
        task<void> parked = body();
        {
            scheduler_lifecycle_test::scoped_current const bound{&standalone};
            parked.handle().resume(); // registered with `standalone`
            for (int i = 0; i < 2000 && standalone.pending_count() == 0; ++i)
                qb::io::async::listener::current.run_once_for(1ms); // the timer fires: queued in `standalone`
            ASSERT_EQ(standalone.pending_count(), 1u);
        }
        scheduler_lifecycle_test::scoped_current const none{nullptr};
        parked = task<void>{}; // destroyed with no scheduler current
    }
    EXPECT_EQ(standalone.pending_count(), 0u) << "the destroyed frame is still queued in the scheduler it registered with";
    EXPECT_FALSE(resumed.load());
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test The thread's live schedulers: a scheduler is known from its construction to its destruction, whichever one of
 *       several goes first, and one beyond the table's 16 is unknown -- its awaiters then fall back on the current
 *       scheduler -- without keeping a slot once it goes.
 * @brief The registry only compares the cached pointer, never follows it (Huly QB-1220): a scheduler left known after
 *        its destruction is followed into freed memory, one forgotten while it lives leaves its frames in its queues.
 */
TEST_F(CoroutineSchedulerTests, ASchedulerIsKnownLiveFromItsConstructionToItsDestruction) {
    using detail::is_live_scheduler;
    auto                      first  = std::make_unique<CoroutineScheduler>();
    auto                      second = std::make_unique<CoroutineScheduler>();
    auto                      third  = std::make_unique<CoroutineScheduler>();
    CoroutineScheduler const *gone   = second.get();
    EXPECT_TRUE(is_live_scheduler(first.get()) && is_live_scheduler(gone) && is_live_scheduler(third.get()));
    second.reset(); // entered before `third`: not the table's last slot
    EXPECT_FALSE(is_live_scheduler(gone)) << "a destroyed scheduler is still known live";
    EXPECT_TRUE(is_live_scheduler(first.get()) && is_live_scheduler(third.get())) << "destroying one forgot another";
    gone = first.get();
    first.reset();
    EXPECT_FALSE(is_live_scheduler(gone));
    EXPECT_TRUE(is_live_scheduler(third.get()));

    std::vector<std::unique_ptr<CoroutineScheduler>> many;
    for (int i = 0; i < 20; ++i)
        many.push_back(std::make_unique<CoroutineScheduler>());
    EXPECT_FALSE(is_live_scheduler(many.back().get())) << "the table holds 16: the rest are unknown";
    std::vector<CoroutineScheduler const *> addresses;
    for (auto const &each : many)
        addresses.push_back(each.get());
    many.clear();
    EXPECT_TRUE(is_live_scheduler(third.get())) << "destroying the others forgot one still alive";
    for (CoroutineScheduler const *each : addresses)
        EXPECT_FALSE(is_live_scheduler(each)) << "a destroyed scheduler is still known live";
    auto const again = std::make_unique<CoroutineScheduler>();
    EXPECT_TRUE(is_live_scheduler(again.get())) << "the schedulers beyond the table kept slots once destroyed";
}

/**
 * @test A temporary listener that ran a coroutine took over as the thread's current scheduler when it created its own;
 *       once it is gone, none is bound, and the next lookup binds the thread's own listener's again.
 * @brief Its reset unbinds only its own scheduler (Huly QB-1220) -- here the current one, so the thread is left with
 *        none until a lookup that binds -- `current()`, `schedule_via_current()`, an actor's spawn -- binds the loop's
 *        again; `current_ptr()` and `run_ready()` do not -- `forget_frame_if_current()` scrubs every live one (QB-1245).
 */
TEST_F(CoroutineSchedulerTests, ATemporaryListenerThatCreatedItsSchedulerLeavesTheThreadsOwnToBeBoundAgain) {
    auto *const sched = &coro_scheduler();
    {
        qb::io::async::listener   temporary;
        CoroutineScheduler *const its = &temporary.coro_scheduler(); // created here
        EXPECT_EQ(CoroutineScheduler::current_ptr(), its) << "its scheduler took over as current";
    }
    EXPECT_EQ(CoroutineScheduler::current_ptr(), nullptr) << "the temporary listener's destroyed scheduler is still bound";
    EXPECT_EQ(&CoroutineScheduler::current(), sched) << "the thread's own scheduler was not bound again";
}

/**
 * @test A scheduler destroyed while it is the thread's current one unbinds itself.
 * @brief A directly-owned scheduler left the thread's current pointer dangling until its owner cleared it, and an
 *        awaiter falling back on "the current scheduler" in that window followed it (Huly QB-1220).
 */
TEST_F(CoroutineSchedulerTests, ASchedulerDestroyedWhileCurrentUnbindsItself) {
    auto *const sched = &coro_scheduler();
    {
        auto standalone = std::make_unique<CoroutineScheduler>();
        CoroutineScheduler::set_current(standalone.get());
        standalone.reset();
        EXPECT_EQ(CoroutineScheduler::current_ptr(), nullptr) << "the destroyed scheduler is still the current one";
    }
    CoroutineScheduler::set_current(sched);
}

namespace scheduler_lifecycle_test {
// A hand-driven frame that parks on `ch` and is woken through its wait list (Huly QB-1245).
inline task<void>
receive_once(channel<int> &ch) {
    (void) co_await ch.recv();
}

// A hand-driven frame that parks on a timer.
inline task<void>
nap(qb::duration d) {
    co_await sleep(d);
}
} // namespace scheduler_lifecycle_test

/**
 * @test A task destroyed unfinished while its frame is queued in a live scheduler that is neither current nor
 *       destroying leaves that scheduler's queue (Huly QB-1245).
 * @brief The frame was woken from a channel's wait list while `queued_in` was current, so it sits in that queue; the
 *        task is destroyed while another scheduler is current. ~task scrubbed only the current scheduler (and the one
 *        destroying its frames): `queued_in` kept the entry, and its next drain read done() of a freed frame -- hidden
 *        from AddressSanitizer by the frame pool, so the queue is counted.
 */
TEST_F(CoroutineSchedulerTests, ATaskDestroyedWhileQueuedInALiveSchedulerNotCurrentLeavesThatQueue) {
    const long                baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>              ch;
    CoroutineScheduler        queued_in;
    CoroutineScheduler        other;
    std::optional<task<void>> held;
    {
        scheduler_lifecycle_test::scoped_current const bound{&queued_in};
        held.emplace(scheduler_lifecycle_test::receive_once(ch));
        held->handle().resume();     // parks on the channel: driven by hand, never spawned
        ASSERT_TRUE(ch.try_send(1)); // wakes it: queued in `queued_in`, the current scheduler
        ASSERT_EQ(queued_in.pending_count(), 1u);
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&other};
        held.reset(); // unfinished, destroyed while `other` is current
    }
    EXPECT_EQ(queued_in.pending_count(), 0u) << "the destroyed frame is still queued in a live scheduler";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test The same with no scheduler current at all -- the window a temporary listener that created its own scheduler
 *       leaves behind (Huly QB-1245): the loop's own scheduler is alive, unbound, and holds the frame.
 */
TEST_F(CoroutineSchedulerTests, ATaskDestroyedWithNoSchedulerCurrentLeavesTheQueueItIsIn) {
    const long                baseline = detail::CoroutineFrameAllocator::live_frames;
    auto *const               sched    = &coro_scheduler();
    channel<int>              ch;
    std::optional<task<void>> held;
    held.emplace(scheduler_lifecycle_test::receive_once(ch));
    held->handle().resume();
    ASSERT_TRUE(ch.try_send(1)); // queued in the thread's own scheduler, current here
    ASSERT_EQ(sched->pending_count(), 1u);
    {
        scheduler_lifecycle_test::scoped_current const none{nullptr};
        held.reset();
    }
    EXPECT_EQ(sched->pending_count(), 0u) << "the destroyed frame is still queued in the loop's scheduler";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A frame whose timer was armed under one scheduler and fired under another -- queued in the second, the
 *       current one when it fired -- leaves the second's queue when its task is destroyed under the first.
 * @brief Its awaiter scrubs the scheduler it registered with; ~task scrubbed the current one, the first again: the
 *        scheduler that queued it kept the entry (Huly QB-1245).
 */
TEST_F(CoroutineSchedulerTests, ATimerFrameQueuedUnderAnotherSchedulerLeavesItWhenItsTaskGoes) {
    const long                baseline = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler        armed_under;
    CoroutineScheduler        fired_under;
    std::optional<task<void>> held;
    {
        scheduler_lifecycle_test::scoped_current const bound{&armed_under};
        held.emplace(scheduler_lifecycle_test::nap(1ms));
        held->handle().resume(); // registered with `armed_under`
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&fired_under};
        for (int i = 0; i < 2000 && fired_under.pending_count() == 0; ++i)
            qb::io::async::listener::current.run_once_for(1ms); // the timer fires: queued in `fired_under`
        ASSERT_EQ(fired_under.pending_count(), 1u);
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&armed_under};
        held.reset();
    }
    EXPECT_EQ(fired_under.pending_count(), 0u) << "the destroyed frame is still queued where its timer put it";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A scheduler beyond the live table -- the 17th of its thread -- destroying its frames while another is current
 *       scrubs a woken frame from its own queue (Huly QB-1245).
 * @brief The inner frame was queued in `beyond` by the channel's hand-off; the cascade from its owned root destroys it
 *        through ~task while `beyond` is neither current nor in the table, so only the destroying scheduler's own
 *        scrub reaches that queue. A design guard: the base scrubbed the destroying scheduler too.
 */
TEST_F(CoroutineSchedulerTests, ASchedulerBeyondTheTableDestroyingItsFramesScrubsAWokenOneFromItsQueue) {
    const long                                       baseline = detail::CoroutineFrameAllocator::live_frames;
    std::vector<std::unique_ptr<CoroutineScheduler>> table_fillers;
    for (int i = 0; i < 16; ++i)
        table_fillers.push_back(std::make_unique<CoroutineScheduler>());
    channel<int>       ch;
    CoroutineScheduler beyond;
    ASSERT_FALSE(detail::is_live_scheduler(&beyond)) << "the table has room: this case needs a scheduler beyond it";
    auto const inner = [&ch]() -> task<void> {
        (void) co_await ch.recv();
    };
    {
        scheduler_lifecycle_test::scoped_current const bound{&beyond};
        beyond.spawn([&inner]() -> task<void> { co_await inner(); });
        beyond.run_ready();          // the root awaits the inner task, parked on the channel
        ASSERT_TRUE(ch.try_send(1)); // wakes it: queued in `beyond`, the current scheduler
        ASSERT_EQ(beyond.pending_count(), 1u);
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{table_fillers.front().get()};
        beyond.destroy_all_suspended(); // the root, then the inner task's frame through its ~task
    }
    EXPECT_EQ(beyond.pending_count(), 0u) << "the destroyed frame is still queued in the scheduler that destroyed it";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A spawned root queued in a live scheduler other than its own -- woken while that one was current -- leaves
 *       that queue when its own scheduler destroys it (Huly QB-1245).
 * @brief A root is destroyed by its scheduler through its handle, never by a `~task`: destroy_all_suspended() scrubbed
 *        its own queues only, and the scheduler that queued the root kept the entry for its next drain to read.
 */
TEST_F(CoroutineSchedulerTests, ASpawnedRootQueuedInAnotherLiveSchedulerLeavesItWhenItsSchedulerDestroysIt) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>       ch;
    CoroutineScheduler owner;
    CoroutineScheduler woken_under;
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        owner.spawn(scheduler_lifecycle_test::receive_once(ch));
        owner.run_ready(); // the root parks on the channel
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&woken_under};
        ASSERT_TRUE(ch.try_send(1)); // wakes it: queued in `woken_under`, the current scheduler
        ASSERT_EQ(woken_under.pending_count(), 1u);
    }
    owner.destroy_all_suspended(); // the root it owns
    EXPECT_EQ(woken_under.pending_count(), 0u) << "the destroyed root is still queued in the scheduler that woke it";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/// @test The same through `cancel_spawned()`, the reclaim every helper uses (Huly QB-1245).
TEST_F(CoroutineSchedulerTests, ASpawnedRootQueuedInAnotherLiveSchedulerLeavesItWhenItsSchedulerCancelsIt) {
    const long              baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>            ch;
    CoroutineScheduler      owner;
    CoroutineScheduler      woken_under;
    std::coroutine_handle<> root;
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        root = owner.spawn_tracked(scheduler_lifecycle_test::receive_once(ch));
        owner.run_ready();
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&woken_under};
        ASSERT_TRUE(ch.try_send(1));
        ASSERT_EQ(woken_under.pending_count(), 1u);
    }
    owner.cancel_spawned(root);
    EXPECT_EQ(woken_under.pending_count(), 0u) << "the cancelled root is still queued in the scheduler that woke it";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

namespace scheduler_lifecycle_test {
/// Bigger than the coroutine frame pool's classes: a frame holding one is freed by the allocator, which
/// AddressSanitizer watches, so a resume or a write into it once freed is reported, not hidden by the pool.
struct beyond_the_pool {
    volatile char bytes[8192];
};

inline task<int>
receive_value(channel<int> &ch) {
    auto const got = co_await ch.recv();
    co_return got ? *got : -1;
}

inline task<int>
receive_value_in_a_big_frame(channel<int> &ch) {
    beyond_the_pool pad{};
    pad.bytes[0]   = 1;
    auto const got = co_await ch.recv();
    co_return got ? *got + pad.bytes[0] - 1 : -1;
}

inline task<void>
receive_once_in_a_big_frame(channel<int> &ch) {
    beyond_the_pool pad{};
    pad.bytes[0] = 1;
    (void) co_await ch.recv();
    pad.bytes[1] = pad.bytes[0];
}
} // namespace scheduler_lifecycle_test

/**
 * @test A spawned coroutine awaiting a task it does not own -- `co_await held`, an lvalue -- destroyed by its scheduler
 *       leaves that task no continuation to its freed frame (Huly QB-1243).
 * @brief The scheduler destroys the root it owns; `held` is its owner's, parked on a channel. Its continuation still
 *        named the destroyed root: the send completed `held`, whose final_suspend resumed the freed frame -- a
 *        heap-use-after-free under AddressSanitizer, the root's frame being bigger than the pool's classes.
 */
TEST_F(CoroutineSchedulerTests, ASpawnedCoroutineAwaitingATaskItDoesNotOwnLeavesItNoContinuationOnceDestroyed) {
    const long               baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>             ch;
    CoroutineScheduler       standalone;
    std::optional<task<int>> held;
    int                      got = 0;
    {
        scheduler_lifecycle_test::scoped_current const bound{&standalone};
        held.emplace(scheduler_lifecycle_test::receive_value(ch));
        standalone.spawn([&held, &got]() -> task<void> {
            scheduler_lifecycle_test::beyond_the_pool pad{};
            pad.bytes[0] = 1;
            got          = co_await *held; // an lvalue: this coroutine does not own the task
            pad.bytes[1] = pad.bytes[0];
        });
        standalone.run_ready(); // the root starts `held`, which parks on the channel
        ASSERT_FALSE(held->done());
        standalone.destroy_all_suspended(); // the root goes; `held` is left to its owner
        EXPECT_FALSE(detail::promise_of(held->handle()).continuation_) << "the task still names the destroyed root";
        ASSERT_TRUE(ch.try_send(7)); // wakes `held`: its completion resumed the freed root
        standalone.run_ready();
        EXPECT_TRUE(held->done());
    }
    EXPECT_EQ(got, 0) << "the destroyed root ran on";
    held.reset();
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test The same across the listener's recycle -- what a `VirtualCore` runs at its teardown -- with the borrowed task
 *       parked on a timer: it wakes after the recycle and finishes without resuming the destroyed root (Huly QB-1243).
 */
TEST_F(CoroutineSchedulerTests, ACoroutineAwaitingATaskItDoesNotOwnLeavesItNoContinuationAcrossTheRecycle) {
    const long                baseline  = detail::CoroutineFrameAllocator::live_frames;
    const std::size_t         abandoned = abandoned_coroutine_frames_total();
    std::optional<task<void>> held;
    bool                      resumed = false;
    held.emplace(scheduler_lifecycle_test::nap(5ms));
    coro_scheduler().spawn([&held, &resumed]() -> task<void> {
        scheduler_lifecycle_test::beyond_the_pool pad{};
        pad.bytes[0] = 1;
        co_await *held; // an lvalue: this coroutine does not own the task
        resumed      = true;
        pad.bytes[1] = pad.bytes[0];
    });
    coro_scheduler().run_ready(); // the root starts `held`, which parks on its timer
    ASSERT_FALSE(held->done());
    qb::io::async::listener::current.recycle_coro_scheduler();
    EXPECT_FALSE(detail::promise_of(held->handle()).continuation_) << "the task still names the destroyed root";
    EXPECT_TRUE(pump_until([&] { return held->done(); })) << "the borrowed task never finished";
    EXPECT_FALSE(resumed) << "the destroyed root ran on";
    held.reset();
    EXPECT_EQ(abandoned_coroutine_frames_total(), abandoned);
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A borrowed task destroyed by its owner while a spawned coroutine awaits it leaves that coroutine nothing to
 *       clear when its scheduler destroys it in turn (Huly QB-1243).
 * @brief A design guard of the borrow: the awaiting coroutine's frame holds a link to the task's frame, which the
 *        task's frame must cut when it goes first -- else the awaiting frame's destruction writes into the freed one.
 */
TEST_F(CoroutineSchedulerTests, ATaskDestroyedWhileACoroutineAwaitsItLeavesThatCoroutineNothingToClear) {
    const long               baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>             ch;
    CoroutineScheduler       standalone;
    std::optional<task<int>> held;
    {
        scheduler_lifecycle_test::scoped_current const bound{&standalone};
        held.emplace(scheduler_lifecycle_test::receive_value_in_a_big_frame(ch));
        standalone.spawn([&held]() -> task<void> { (void) co_await *held; });
        standalone.run_ready(); // `held` parks on the channel, awaited by the root
        held.reset();           // its owner destroys it, unfinished: the root waits on nothing now
        standalone.destroy_all_suspended();
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A borrowed task that outlives the coroutine it completed for -- the ordinary path -- keeps no link to that
 *       coroutine's freed frame (Huly QB-1243).
 * @brief A design guard of the borrow: the awaiting coroutine resumes, finishes and is freed; the task is destroyed by
 *        its owner afterwards and must not write into the freed frame.
 */
TEST_F(CoroutineSchedulerTests, ABorrowedTaskOutlivingTheCoroutineItCompletedForKeepsNoLinkToIt) {
    const long               baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>             ch;
    CoroutineScheduler       standalone;
    std::optional<task<int>> held;
    int                      got = 0;
    {
        scheduler_lifecycle_test::scoped_current const bound{&standalone};
        held.emplace(scheduler_lifecycle_test::receive_value(ch));
        standalone.spawn([&held, &got]() -> task<void> {
            scheduler_lifecycle_test::beyond_the_pool pad{};
            pad.bytes[0] = 1;
            got          = co_await *held + pad.bytes[0] - 1;
        });
        standalone.run_ready();
        ASSERT_TRUE(ch.try_send(7));
        for (int i = 0; i < 4 && standalone.has_work(); ++i)
            standalone.run_ready(); // `held` completes, the root resumes, finishes and is freed
        EXPECT_EQ(got, 7);
        EXPECT_TRUE(held->done());
        held.reset(); // after the root it completed for is gone
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/// @test The same for a `task<void>`, whose promise is its own type (Huly QB-1243).
TEST_F(CoroutineSchedulerTests, AVoidTaskDestroyedWhileACoroutineAwaitsItLeavesThatCoroutineNothingToClear) {
    const long                baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>              ch;
    CoroutineScheduler        standalone;
    std::optional<task<void>> held;
    {
        scheduler_lifecycle_test::scoped_current const bound{&standalone};
        held.emplace(scheduler_lifecycle_test::receive_once_in_a_big_frame(ch));
        standalone.spawn([&held]() -> task<void> { co_await *held; });
        standalone.run_ready(); // `held` parks on the channel, awaited by the root
        held.reset();           // its owner destroys it, unfinished
        standalone.destroy_all_suspended();
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

namespace scheduler_lifecycle_test {
/**
 * Huly QB-1238. `make(done)` builds a task driven by hand that awaits a helper spawning frames the scheduler owns --
 * a combinator, a timeout, a cancellable operation or sleep, a scope's timed join -- over a short nap, and sets
 * `*done` once resumed. `first` parks on its helper; the reset destroys the helper's frames, whose handles the
 * helper's shared state kept; `second`, the same helper again, is handed the blocks the reset freed by the frame
 * pool; `first`'s owner destroys it, and its helper's teardown reclaims its frames through those handles: a stale
 * one destroyed a frame of `second`, which never finished.
 */
template <typename Make>
void
expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler(Make make, bool started = true) {
    const long                baseline    = detail::CoroutineFrameAllocator::live_frames;
    bool                      second_done = false;
    std::optional<task<void>> first;
    std::optional<task<void>> second;
    first.emplace(make(nullptr));
    second.emplace(make(&second_done));
    first->handle().resume(); // the helper spawns its frames, the scheduler's
    if (started)
        coro_scheduler().run_ready();                        // they run: the inner naps park on their timers (else destroyed unstarted)
    qb::io::async::listener::current.reset_coro_scheduler(); // destroys them; `first` stays parked on its helper
    second->handle().resume();                               // the same helper: its frames take the blocks the reset freed
    first.reset();                                           // its helper's teardown reclaims the frames its handles named
    EXPECT_TRUE(pump_until([&] { return second_done; })) << "the first helper's teardown destroyed a frame of the second";
    second.reset();
    EXPECT_TRUE(pump_until([&] { return detail::CoroutineFrameAllocator::live_frames == baseline; })) << "a frame outlived both tasks";
}
} // namespace scheduler_lifecycle_test

/// @test `when_all` over tasks, kept across the reset (Huly QB-1238): see the driver above.
TEST_F(CoroutineSchedulerTests, AWhenAllKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        (void) co_await when_all(scheduler_lifecycle_test::nap(5ms), scheduler_lifecycle_test::nap(5ms));
        if (done)
            *done = true;
    });
}

namespace scheduler_lifecycle_test {
inline task<int>
nap_then(qb::duration d, int value) {
    co_await sleep(d);
    co_return value;
}

/// A scope's worker in a frame beyond the pool: the scope's own frames are then the only pooled ones it allocates.
inline task<void>
wait_closed(std::shared_ptr<channel<int>> ch) {
    beyond_the_pool pad{};
    pad.bytes[0] = 1;
    (void) co_await ch->recv();
    pad.bytes[1] = pad.bytes[0];
}

/// Joins `scope` within 5 ms -- it times out, its worker waiting for `ch` to close -- then closes `ch` and joins.
inline task<bool>
time_out_then_join(coroutine_scope &scope, std::shared_ptr<channel<int>> ch, task<bool> timed_join) {
    bool const joined = co_await timed_join;
    ch->close();
    co_await scope.join_all();
    co_return !joined;
}

inline std::vector<task<void>>
two_naps() {
    std::vector<task<void>> naps;
    naps.push_back(nap(5ms));
    naps.push_back(nap(5ms));
    return naps;
}
} // namespace scheduler_lifecycle_test

/// @test `when_all` over a vector of tasks, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, AWhenAllOverAVectorKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        (void) co_await when_all(scheduler_lifecycle_test::two_naps());
        if (done)
            *done = true;
    });
}

/// @test `when_any` over tasks, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, AWhenAnyKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        (void) co_await when_any(scheduler_lifecycle_test::nap(5ms), scheduler_lifecycle_test::nap(5ms));
        if (done)
            *done = true;
    });
}

/// @test `when_any` over a vector of tasks, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, AWhenAnyOverAVectorKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        (void) co_await when_any(scheduler_lifecycle_test::two_naps());
        if (done)
            *done = true;
    });
}

/// @test A timeout over a `task<void>`, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ATimeoutKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        co_await coro_with_timeout(scheduler_lifecycle_test::nap(5ms), 10s);
        if (done)
            *done = true;
    });
}

/// @test A timeout over a `task<int>`, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ATimeoutOverAValueKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        int const value = co_await coro_with_timeout(scheduler_lifecycle_test::nap_then(5ms, 7), 10s);
        if (done)
            *done = value == 7;
    });
}

/// @test `make_cancellable()` over a `task<void>`, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ACancellableOperationKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        co_await make_cancellable(scheduler_lifecycle_test::nap(5ms), cancellation_token{});
        if (done)
            *done = true;
    });
}

/// @test `make_cancellable()` over a `task<int>`, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ACancellableValueKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        int const value = co_await make_cancellable(scheduler_lifecycle_test::nap_then(5ms, 7), cancellation_token{});
        if (done)
            *done = value == 7;
    });
}

/// @test `cancellable_sleep()`, kept across the reset (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ACancellableSleepKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        co_await cancellable_sleep(5ms, cancellation_token{});
        if (done)
            *done = true;
    });
}

/// @test `with_deadline()`, kept across the reset (Huly QB-1238): its `when_any` branches and its deadline timer.
TEST_F(CoroutineSchedulerTests, ADeadlineKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        co_await with_deadline(scheduler_lifecycle_test::nap(5ms), std::chrono::steady_clock::now() + 10s);
        if (done)
            *done = true;
    });
}

/**
 * @test A scope's timed join, kept across the reset (Huly QB-1238): its timer's handle named a frame of the next one.
 * @brief The same case as the driver above, laid out so the frame pool's reuse is certain: the workers' frames are
 *        beyond the pool, and the second task builds its join's frame before the reset, then parks on a gate. After
 *        the reset its only pooled frames are the two the scheduler owns -- its worker's wrapper and its join's
 *        timer -- and they take the two blocks the reset freed; the first task's teardown reclaimed its timer through
 *        the stale handle, destroying one of them, and the second never finished.
 */
TEST_F(CoroutineSchedulerTests, AScopesTimedJoinKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    const long                baseline    = detail::CoroutineFrameAllocator::live_frames;
    bool                      second_done = false;
    channel<int>              gate;
    std::optional<task<void>> first;
    std::optional<task<void>> second;
    first.emplace([]() -> task<void> {
        auto            ch = std::make_shared<channel<int>>();
        coroutine_scope scope;
        scope.spawn(scheduler_lifecycle_test::wait_closed(ch));
        (void) co_await scheduler_lifecycle_test::time_out_then_join(scope, ch, scope.join_all_for(5ms));
    }());
    second.emplace([](channel<int> &gate, bool &done) -> task<void> {
        auto            ch = std::make_shared<channel<int>>();
        coroutine_scope scope;
        task<bool>      join = scheduler_lifecycle_test::time_out_then_join(scope, ch, scope.join_all_for(5ms));
        (void) co_await gate.recv();                            // parked across the reset: its frames exist already
        scope.spawn(scheduler_lifecycle_test::wait_closed(ch)); // after it: the worker's wrapper, pooled
        done = co_await join;                                   // the timer, pooled
    }(gate, second_done));
    first->handle().resume();                                // its worker and its join's timer, the scheduler's
    coro_scheduler().run_ready();                            // the worker parks on its channel, the timer on its sleep
    second->handle().resume();                               // builds its join, parks on the gate
    qb::io::async::listener::current.reset_coro_scheduler(); // destroys the first task's worker and timer
    ASSERT_TRUE(gate.try_send(1));
    for (int i = 0; i < 4; ++i)
        coro_scheduler().run_ready(); // the second spawns its worker and its timer, in the blocks the reset freed
    first.reset();                    // its join's teardown reclaims its timer through the handle it kept
    EXPECT_TRUE(pump_until([&] { return second_done; })) << "the first join's teardown destroyed a frame of the second";
    second.reset();
    EXPECT_TRUE(pump_until([&] { return detail::CoroutineFrameAllocator::live_frames == baseline; })) << "a frame outlived both tasks";
}

/**
 * @test A shared task kept across the reset (Huly QB-1238): its runner keeps no handle, owns its inner task and goes
 *       with it, so the reset leaves the awaiting task unresumed and safe to destroy. A design guard.
 */
TEST_F(CoroutineSchedulerTests, ASharedTaskKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        co_await make_shared_task(scheduler_lifecycle_test::nap(5ms));
        if (done)
            *done = true;
    });
}

/**
 * @test A scope's untimed join kept across the reset (Huly QB-1238): the scope keeps no handle of its workers, so the
 *       reset leaves the joining task unresumed and safe to destroy. A design guard.
 */
TEST_F(CoroutineSchedulerTests, AScopesJoinKeptAcrossTheResetReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler([](bool *done) -> task<void> {
        coroutine_scope scope;
        scope.spawn(scheduler_lifecycle_test::nap(5ms));
        co_await scope.join_all();
        if (done)
            *done = true;
    });
}

/**
 * @test The deadline timer of `with_deadline()`, destroyed by the reset, leaves its state no handle to it (Huly
 *       QB-1238).
 * @brief Read on the state itself: in the case through `with_deadline()` above, its `when_any` branches already
 *        decide the outcome, so a stale timer handle there reaches a frame of the next scheduler only when the frame
 *        pool happens to place one of them -- not the timer -- at its address.
 */
TEST_F(CoroutineSchedulerTests, ADeadlineTimerDestroyedByTheResetLeavesItsStateNoHandleToIt) {
    const long               baseline = detail::CoroutineFrameAllocator::live_frames;
    auto                     state    = std::make_shared<detail::with_deadline_timeout_state>();
    std::optional<task<int>> parked;
    parked.emplace(detail::with_deadline_run_timeout(state, std::chrono::steady_clock::now() + 10s, cancellation_token{}));
    parked->handle().resume(); // its awaiter spawns the deadline timer, the scheduler's
    ASSERT_TRUE(state->timer_handle);
    coro_scheduler().run_ready(); // the timer parks on its sleep
    qb::io::async::listener::current.reset_coro_scheduler();
    EXPECT_FALSE(state->timer_handle) << "the deadline's state still names the timer frame the reset destroyed";
    parked.reset();
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test `when_all`'s branches destroyed by the reset before they ever ran leave its state no handle to them (Huly
 *       QB-1238): the reset comes between the spawn and the first `run_ready()`.
 */
TEST_F(CoroutineSchedulerTests, AWhenAllWhoseBranchesTheResetDestroyedUnstartedReachesNoFrameOfTheNextScheduler) {
    scheduler_lifecycle_test::expect_a_kept_helper_reaches_no_frame_of_the_next_scheduler(
        [](bool *done) -> task<void> {
            (void) co_await when_all(scheduler_lifecycle_test::nap(5ms), scheduler_lifecycle_test::nap(5ms));
            if (done)
                *done = true;
        },
        false);
}

namespace scheduler_lifecycle_test {
inline task<void>
send_then_receive(std::shared_ptr<channel<int>> in, std::shared_ptr<channel<int>> out, std::optional<int> *got) {
    (void) co_await in->send(1);
    if (auto const value = co_await out->recv())
        *got = *value;
}

inline task<void>
collect_into(async_stream<int> stream, std::vector<int> *got, bool *done) {
    *got  = co_await stream.collect();
    *done = true;
}

inline int
plus_one(int value) {
    return value + 1;
}
} // namespace scheduler_lifecycle_test

/**
 * @test A pipeline whose worker the reset destroyed before it ever ran leaves its lease no handle to it (Huly
 *       QB-1238): the worker's slot guard is its parameter.
 * @brief The worker built its slot guard first in its body, so a frame destroyed unstarted left the slot behind; the
 *        second pipeline's worker took the block the reset freed, and the first pipeline's last handles ended their
 *        lease through the stale slot -- destroying the second's worker, whose pipeline never answered.
 */
TEST_F(CoroutineSchedulerTests, APipelineWhoseWorkerTheResetDestroyedUnstartedReachesNoFrameOfTheNextScheduler) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;
    auto       first    = make_pipeline<int, int>(&scheduler_lifecycle_test::plus_one); // its worker spawned, never run
    qb::io::async::listener::current.reset_coro_scheduler();                            // destroys that worker before it ever ran
    auto second = make_pipeline<int, int>(&scheduler_lifecycle_test::plus_one);         // its worker: the block the reset freed
    first       = {};                                                                   // the last handles: the lease ends
    std::optional<int> got;
    task<void>         consumer = scheduler_lifecycle_test::send_then_receive(second.first, second.second, &got);
    consumer.handle().resume();
    EXPECT_TRUE(pump_until([&] { return got.has_value(); })) << "the first pipeline's lease destroyed the second's worker";
    EXPECT_EQ(got.value_or(0), 2);
    consumer = task<void>{};
    second   = {};
    EXPECT_TRUE(pump_until([&] { return detail::CoroutineFrameAllocator::live_frames == baseline; })) << "a frame outlived both pipelines";
}

/// @test The same for `async_stream::backpressure()`'s producer, spawned at once (Huly QB-1238).
TEST_F(CoroutineSchedulerTests, ABackpressuredStreamWhoseProducerTheResetDestroyedUnstartedReachesNoFrameOfTheNextScheduler) {
    const long                       baseline = detail::CoroutineFrameAllocator::live_frames;
    std::optional<async_stream<int>> first{async_stream<int>::from_vector({1, 2, 3}).backpressure(2)};
    qb::io::async::listener::current.reset_coro_scheduler(); // destroys its producer before it ever ran
    std::optional<async_stream<int>> second{async_stream<int>::from_vector({1, 2, 3}).backpressure(2)};
    first.reset(); // the first stream's last handle: its lease ends
    std::vector<int> got;
    bool             done     = false;
    task<void>       consumer = scheduler_lifecycle_test::collect_into(std::move(*second), &got, &done);
    second.reset();
    consumer.handle().resume();
    EXPECT_TRUE(pump_until([&] { return done; })) << "the first stream's lease destroyed the second's producer";
    EXPECT_EQ(got, (std::vector<int>{1, 2, 3}));
    consumer = task<void>{};
    EXPECT_TRUE(pump_until([&] { return detail::CoroutineFrameAllocator::live_frames == baseline; })) << "a frame outlived both streams";
}

namespace scheduler_lifecycle_test {
/// Has `owner` cancel `*root` when it goes: a coroutine holding one -- a local, or its parameter -- reclaims another
/// root from inside its own destruction, as a helper's teardown does.
struct cancels_on_destruction {
    CoroutineScheduler      *owner;
    std::coroutine_handle<> *root;
    cancels_on_destruction(CoroutineScheduler *o, std::coroutine_handle<> *r) noexcept
        : owner(o)
        , root(r) {}
    cancels_on_destruction(cancels_on_destruction &&other) noexcept
        : owner(std::exchange(other.owner, nullptr))
        , root(other.root) {}
    cancels_on_destruction &operator=(cancels_on_destruction &&) = delete;
    ~cancels_on_destruction() {
        if (owner && *root)
            owner->cancel_spawned(std::exchange(*root, {}));
    }
};

/// A spawned root that parks until destroyed, then has `owner` cancel `*root`.
inline task<void>
parked_cancelling_on_destruction(CoroutineScheduler *owner, std::coroutine_handle<> *root) {
    cancels_on_destruction const guard{owner, root};
    co_await std::suspend_always{};
}

/// The thread's live table filled: the 16 schedulers it holds, the first of them known live.
inline std::vector<std::unique_ptr<CoroutineScheduler>>
fill_the_table() {
    std::vector<std::unique_ptr<CoroutineScheduler>> fillers;
    for (int i = 0; i < 16; ++i)
        fillers.push_back(std::make_unique<CoroutineScheduler>());
    return fillers;
}
} // namespace scheduler_lifecycle_test

/**
 * @test A spawned root queued in the current scheduler, one beyond the live table, leaves that queue when its owner
 *       destroys it (Huly QB-1245).
 * @brief The root's channel woke it while `beyond` -- its thread's 17th scheduler, which the table has no slot for -- was
 *        current, and `beyond` is still current when the owner's reset destroys it: only a scrub of the current
 *        scheduler reaches that queue. The base reached it through forget_frame_if_current(); the reclaim's own pass over
 *        the table did not (a regression the focused review of QB-1245's optimisation found).
 */
TEST_F(CoroutineSchedulerTests, ASpawnedRootQueuedInACurrentSchedulerBeyondTheTableLeavesItWhenItsOwnerDestroysIt) {
    const long          baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>        ch;
    auto const          fillers = scheduler_lifecycle_test::fill_the_table();
    CoroutineScheduler &owner   = *fillers.front();
    CoroutineScheduler  beyond;
    ASSERT_TRUE(detail::is_live_scheduler(&owner));
    ASSERT_FALSE(detail::is_live_scheduler(&beyond)) << "the table has room: this case needs a scheduler beyond it";
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        owner.spawn(scheduler_lifecycle_test::receive_once(ch));
        owner.run_ready(); // the root parks on the channel
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&beyond};
        ASSERT_TRUE(ch.try_send(1)); // wakes it: queued in `beyond`, the current scheduler
        ASSERT_EQ(beyond.pending_count(), 1u);
        owner.destroy_all_suspended(); // `beyond` still current
    }
    EXPECT_EQ(beyond.pending_count(), 0u) << "the destroyed root is still queued in the current scheduler";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/// @test The same through the owner's `cancel_spawned()` (Huly QB-1245).
TEST_F(CoroutineSchedulerTests, ASpawnedRootQueuedInACurrentSchedulerBeyondTheTableLeavesItWhenItsOwnerCancelsIt) {
    const long              baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>            ch;
    auto const              fillers = scheduler_lifecycle_test::fill_the_table();
    CoroutineScheduler     &owner   = *fillers.front();
    CoroutineScheduler      beyond;
    std::coroutine_handle<> root;
    ASSERT_TRUE(detail::is_live_scheduler(&owner));
    ASSERT_FALSE(detail::is_live_scheduler(&beyond)) << "the table has room: this case needs a scheduler beyond it";
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        root = owner.spawn_tracked(scheduler_lifecycle_test::receive_once(ch));
        owner.run_ready();
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&beyond};
        ASSERT_TRUE(ch.try_send(1));
        ASSERT_EQ(beyond.pending_count(), 1u);
        owner.cancel_spawned(root);
    }
    EXPECT_EQ(beyond.pending_count(), 0u) << "the cancelled root is still queued in the current scheduler";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A spawned root queued in a scheduler beyond the live table leaves that queue when its owner cancels it from
 *       inside that scheduler's reset (Huly QB-1245).
 * @brief `beyond` destroys its parked root, whose destruction has the owner cancel a root `beyond` had queued: `beyond`
 *        is neither current nor in the table, only the destroying scheduler -- which a reclaim's scrub must reach as
 *        forget_frame_if_current() does.
 */
TEST_F(CoroutineSchedulerTests, ASpawnedRootQueuedInADestroyingSchedulerBeyondTheTableLeavesItWhenItsOwnerCancelsIt) {
    const long              baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>            ch;
    auto const              fillers   = scheduler_lifecycle_test::fill_the_table();
    CoroutineScheduler     &owner     = *fillers.front();
    CoroutineScheduler     &bystander = *fillers.back();
    CoroutineScheduler      beyond;
    std::coroutine_handle<> root;
    ASSERT_TRUE(detail::is_live_scheduler(&owner));
    ASSERT_FALSE(detail::is_live_scheduler(&beyond)) << "the table has room: this case needs a scheduler beyond it";
    {
        scheduler_lifecycle_test::scoped_current const bound{&beyond};
        beyond.spawn(scheduler_lifecycle_test::parked_cancelling_on_destruction(&owner, &root));
        beyond.run_ready(); // parks until `beyond` destroys it
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        root = owner.spawn_tracked(scheduler_lifecycle_test::receive_once(ch));
        owner.run_ready(); // parks on the channel
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&beyond};
        ASSERT_TRUE(ch.try_send(1)); // queued in `beyond`
        ASSERT_EQ(beyond.pending_count(), 1u);
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&bystander};
        beyond.destroy_all_suspended(); // its root goes, and has the owner cancel `root`
    }
    ASSERT_FALSE(root) << "the owner did not cancel the root";
    EXPECT_EQ(beyond.pending_count(), 0u) << "the cancelled root is still queued in the scheduler destroying its frames";
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

namespace scheduler_lifecycle_test {
/// A spawned root in a frame beyond the pool that parks until resumed by hand, then finishes.
inline task<void>
root_resumed_by_hand_in_a_big_frame() {
    beyond_the_pool pad{};
    pad.bytes[0] = 1;
    co_await std::suspend_always{};
    pad.bytes[1] = pad.bytes[0];
}
} // namespace scheduler_lifecycle_test

/**
 * @test A spawned root that finishes under another live scheduler leaves its owner no handle to the freed frame
 *       (Huly QB-1243).
 * @brief Its waker queued it in the scheduler current when it fired, which resumed it; it finished there, and its
 *        destruction was deferred to that scheduler, the current one at its final suspension -- which freed it and
 *        erased it from its OWN ownership set, not the owner's. The owner's next destroy_all_suspended() read done() of
 *        the freed frame (a heap-use-after-free under AddressSanitizer, the frame being beyond the pool), and destroyed
 *        it a second time if the pool had handed the block to a frame still running.
 */
TEST_F(CoroutineSchedulerTests, ASpawnedRootFinishedUnderAnotherSchedulerLeavesItsOwnerNoHandleToIt) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    channel<int>       ch;
    CoroutineScheduler owner;
    CoroutineScheduler finished_under;
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        owner.spawn(scheduler_lifecycle_test::receive_once_in_a_big_frame(ch));
        owner.run_ready(); // the root parks on the channel
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&finished_under};
        ASSERT_TRUE(ch.try_send(1)); // queued in `finished_under`
        finished_under.run_ready();  // resumed, finished, freed by `finished_under`
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
    EXPECT_TRUE(owner.dump().empty()) << "the owner still counts a root another scheduler freed";
    owner.destroy_all_suspended(); // read done() of the freed root
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test A finished root still waiting in another scheduler's deferred destructions is destroyed once when its owner
 *       cancels it (Huly QB-1243).
 * @brief It finished outside any drain, resumed by hand while another scheduler was current (as an `ask` reply resumes a
 *        frame inline), so its destruction waited in that scheduler's list; its owner's cancel_spawned() scrubbed only
 *        its own list before freeing it, and the other scheduler's next drain freed it again.
 */
TEST_F(CoroutineSchedulerTests, AFinishedRootWaitingInAnotherSchedulersDeferredListIsDestroyedOnce) {
    const long              baseline = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler      owner;
    CoroutineScheduler      finished_under;
    std::coroutine_handle<> root;
    {
        scheduler_lifecycle_test::scoped_current const bound{&owner};
        root = owner.spawn_tracked(scheduler_lifecycle_test::root_resumed_by_hand_in_a_big_frame());
        owner.run_ready(); // the root parks
    }
    {
        scheduler_lifecycle_test::scoped_current const bound{&finished_under};
        root.resume(); // finishes: its destruction deferred to `finished_under`, not yet drained
        ASSERT_TRUE(root.done());
    }
    owner.cancel_spawned(root); // the owner reclaims it
    {
        scheduler_lifecycle_test::scoped_current const bound{&finished_under};
        finished_under.run_ready(); // the deferred destruction: a second free
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

/**
 * @test The same through the other scheduler's teardown: a root it never drained, freed by its destructor's drain,
 *       leaves its owner no handle to it either (Huly QB-1243).
 */
TEST_F(CoroutineSchedulerTests, ARootFreedByAnotherSchedulersTeardownLeavesItsOwnerNoHandleToIt) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    CoroutineScheduler owner;
    {
        auto                    finished_under = std::make_unique<CoroutineScheduler>();
        std::coroutine_handle<> root;
        {
            scheduler_lifecycle_test::scoped_current const bound{&owner};
            root = owner.spawn_tracked(scheduler_lifecycle_test::root_resumed_by_hand_in_a_big_frame());
            owner.run_ready();
        }
        {
            scheduler_lifecycle_test::scoped_current const bound{finished_under.get()};
            root.resume(); // finishes: deferred to `finished_under`
        }
        finished_under.reset(); // its teardown drain frees the root
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
    EXPECT_TRUE(owner.dump().empty()) << "the owner still counts a root another scheduler freed";
    owner.destroy_all_suspended(); // read done() of the freed root
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline);
}

namespace scheduler_lifecycle_test {
/// A spawned root that parks until resumed by hand and finishes; its frame, when it goes, has the guard cancel.
inline task<void>
finished_cancelling_on_destruction(cancels_on_destruction guard) {
    co_await std::suspend_always{};
    (void) guard;
}
} // namespace scheduler_lifecycle_test

/**
 * @test A teardown drain whose destruction of one finished root cancels another, still in its list, destroys each once
 *       (Huly QB-1243).
 * @brief The drain walked its deferred destructions with a range-for: the first root's frame, destroyed, had the
 *        scheduler cancel the second, which erased it from that very list and freed it, and the loop read the moved
 *        tail past the list's new end -- the second root, destroyed a second time. The drain now pops them one at a time,
 *        as run_ready() does, so a destruction may erase, null or defer another. The second frame is beyond the pool.
 */
TEST_F(CoroutineSchedulerTests, ATeardownDrainWhoseDestructionCancelsAnotherFinishedRootDestroysEachOnce) {
    const long baseline = detail::CoroutineFrameAllocator::live_frames;
    {
        auto                    doomed = std::make_unique<CoroutineScheduler>();
        std::coroutine_handle<> first;
        std::coroutine_handle<> second;
        {
            scheduler_lifecycle_test::scoped_current const bound{doomed.get()};
            first  = doomed->spawn_tracked(scheduler_lifecycle_test::finished_cancelling_on_destruction({doomed.get(), &second}));
            second = doomed->spawn_tracked(scheduler_lifecycle_test::root_resumed_by_hand_in_a_big_frame());
            doomed->run_ready(); // both park
            first.resume();      // each finishes: its destruction deferred, in this order
            second.resume();
            ASSERT_TRUE(first.done());
            ASSERT_TRUE(second.done());
        }
        doomed.reset(); // the teardown drain
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, baseline) << "a frame was freed twice, or never";
}
