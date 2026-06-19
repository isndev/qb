/**
 * @file qb/io/async/coroutine/scheduler.h
 * @brief Coroutine scheduler for libev integration
 *
 * Manages coroutine execution and lifecycle, tracks ready coroutines, and integrates
 * with the libev event loop for efficient suspend/resume operations.
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
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_SCHEDULER_H
#define QB_IO_ASYNC_COROUTINE_SCHEDULER_H

#include <cassert>
#include <coroutine>
#include <deque>
#include <unordered_set>
#include <vector>
#include <ev/ev++.h>
// No <mutex>, no atomics: the scheduler is strictly mono-thread. Every
// caller — libev callbacks, coroutine bodies after `resume()`, awaiters'
// `await_suspend`, or `schedule_via_current` — runs on the VirtualCore
// (or listener) thread that owns this scheduler. We replaced the original
// lock-free MPSC queue by a plain `std::deque` (Finding 2.B.10): one
// allocation per ~8 nodes vs. one allocation per node, no atomics, no
// cache-line contention, and ~2x less overhead on the hot path
// `schedule_resume -> pop` according to the coroutine benchmark harness.
// <atomic> kept only for #ifdef QB_DEBUG_COROUTINES next_id in task.h.

/** Enable scheduler/listener lifecycle debug traces (destructor, register_suspended, clear, reset).
 *  Build with -DQB_DEBUG_CORO_LIFECYCLE=1 to trace teardown and suspended counts. */
#include <cstdio>
#if (defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE) || (defined(QB_DEBUG_CORO_LIFECYCLE) && QB_DEBUG_CORO_LIFECYCLE)
// Standard C++20 __VA_OPT__ elides the comma when no trailing args are passed
// (MSVC needs the conformant preprocessor /Zc:preprocessor, enabled by qb's build).
#define QB_SCHED_TRACE(fmt, ...) std::fprintf(stderr, "[sched] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define QB_SCHED_TRACE(fmt, ...) ((void) 0)
#endif

// Include task.h for spawn() implementation - must be after CoroutineScheduler declaration
#include "task.h"

namespace qb::io::async {

/**
 * @brief Manages coroutine execution and lifecycle
 *
 * The CoroutineScheduler is responsible for:
 * - Tracking coroutines ready to resume
 * - Managing coroutine spawning and completion
 * - Integrating with the libev event loop
 * - Providing thread-local scheduler access
 *
 * Each thread (VirtualCore) has its own scheduler instance.
 *
 * USAGE GUIDELINES:
 * ================
 *
 * Spawning Coroutines:
 * @code
 * auto t = my_coroutine();
 * qb::io::async::coro_scheduler().spawn(std::move(t));
 * @endcode
 *
 * Running Ready Coroutines:
 * @code
 * qb::io::async::coro_scheduler().run_ready();  // Process all ready coroutines
 * @endcode
 *
 * Checking Active Coroutines:
 * @code
 * size_t active = qb::io::async::coro_scheduler().active_count();
 * @endcode
 *
 * CRITICAL: Ownership Semantics
 * ==============================
 * - spawn() takes ownership of the coroutine handle
 * - Spawned coroutines run to completion even if the original task is destroyed
 * - schedule_resume() does NOT take ownership (used for continuations)
 *
 * Thread Safety (Finding 2.B.9):
 * ================================
 * - **Strictly mono-thread.** One `CoroutineScheduler` instance belongs to
 *   exactly one thread: the VirtualCore worker thread or the listener's
 *   I/O thread that constructed it. Every entry point (`spawn`,
 *   `schedule_resume`, `enqueue_for_later`, `run_ready`, dtor) assumes
 *   that thread.
 * - Pushing from a **different** thread is undefined behavior. If cross-
 *   thread wake-ups are needed (e.g. a background worker completing a
 *   promise), post a message via the `Actor` mailbox instead — that is
 *   the dedicated MPSC path in qb-core.
 * - Internally: `ready_queue_` is a `std::deque<ready_item>` (no atomics,
 *   no mutex), `in_flight_` and `suspended_coroutines_` are plain
 *   `std::unordered_set<void*>`.
 * - `run_ready()` must not be called re-entrantly (see the `in_run_ready_`
 *   guard inside the implementation).
 * - Use separate scheduler instances per thread.
 *
 * @ingroup Coroutine
 */
class CoroutineScheduler {
public:
    /**
     * @brief Construct with event loop
     * @param loop The libev event loop
     */
    explicit CoroutineScheduler(ev::loop_ref loop = ev::get_default_loop())
        : loop_(loop) {}

    /**
     * @brief Destructor
     *
     * Drains and destroys all coroutines in the ready queue only. Coroutines
     * that are suspended (waiting on I/O or timers) are NOT destroyed, because
     * their libev watchers are still active; destroying those handles would
     * run awaiter destructors that call `ev_*_stop()`, which can cause
     * use-after-free or double-free if the loop is being torn down or if
     * callbacks are in flight.
     *
     * Finding 2.B.8 — decision log:
     *   Destroying suspended frames here would be *worse* than leaking
     *   them because:
     *     1. libev watchers registered in the awaiter still reference the
     *        frame via `ev_watcher::data`. If the loop iterates once more
     *        (e.g. another listener on this thread), it would dispatch to
     *        freed memory.
     *     2. The frame's awaiter may hold non-trivial RAII (cancellation
     *        tokens, shared_ptrs) whose destruction order is not the one
     *        intended by the user when they wrote their coroutine.
     *   The correct lifecycle is therefore: **stop the event loop first**,
     *   then destroy the scheduler. The listener does this on its own
     *   destruction. Suspended frames at process exit are reclaimed by
     *   the OS (they are by definition not going to make progress).
     *
     *   In debug builds we emit a one-line warning if any suspended frames
     *   are left at teardown, so misuse of the lifecycle surfaces during
     *   tests.
     */
    ~CoroutineScheduler() {
        QB_SCHED_TRACE("~CoroutineScheduler() begin this=%p", (void *) this);
        std::size_t ready_drained = 0;
        while (!ready_queue_.empty()) {
            ready_item item = ready_queue_.front();
            ready_queue_.pop_front();
            // Only destroy frames the scheduler owns (spawned). Non-owned
            // entries are continuations whose frame is owned by a task<T>
            // object elsewhere — destroying them here would double-free when
            // that task's destructor runs later. Leaking a queued continuation
            // at teardown is strictly safer.
            if (item.handle && !item.handle.done() && owned_frames_.erase(item.handle.address())) {
                QB_SCHED_TRACE("  destroy ready handle=%p owned=%d", (void *) item.handle.address(), item.owned);
                item.handle.destroy();
            }
            ++ready_drained;
        }
        QB_SCHED_TRACE("  drained ready_queue: %zu items", ready_drained);
        (void) ready_drained;
        // Destroy frames deferred by completed spawned coroutines that were not
        // drained by a final run_ready() (e.g. the loop completing them on its
        // last tick before teardown). They are suspended at final_suspend.
        for (auto h : frames_to_destroy_) {
            if (h) {
                owned_frames_.erase(h.address());
                h.destroy();
            }
        }
        frames_to_destroy_.clear();
#ifndef NDEBUG
        if (!suspended_coroutines_.empty()) {
            std::fprintf(stderr,
                         "[coro][warn] ~CoroutineScheduler() leaked %zu suspended frames — "
                         "stop the event loop before destroying the scheduler.\n",
                         suspended_coroutines_.size());
        }
#endif
        QB_SCHED_TRACE("  clearing suspended_coroutines_ (count=%zu), NOT destroying handles", suspended_coroutines_.size());
        suspended_coroutines_.clear();
        in_flight_.clear();
        // Any owned frames still here are suspended (not in the ready queue) and
        // are intentionally leaked at teardown per Finding 2.B.8 — just drop the
        // bookkeeping; the OS reclaims them at process/thread exit.
        owned_frames_.clear();
        QB_SCHED_TRACE("~CoroutineScheduler() end this=%p", (void *) this);
    }

    /**
     * @brief Spawn a new coroutine
     *
     * The coroutine is added to the ready queue and will be resumed
     * on the next run_ready() call. The task's handle is transferred
     * to the scheduler (ownership is taken from the task).
     *
     * CRITICAL: Ownership Transfer
     * ============================
     * - The scheduler takes ownership of the coroutine handle
     * - The spawned coroutine will run to completion even if the original
     *   task object is destroyed
     * - Always use std::move when passing the task
     *
     * Example:
     * @code
     * auto t = my_coroutine();
     * coro_scheduler().spawn(std::move(t));  // t is now empty
     * @endcode
     *
     * @param t The coroutine task to spawn (moved from)
     */
    void spawn(task<void> &&t);

    /**
     * @brief Spawn a callable (lambda/functor) as a coroutine — closure is owned.
     *
     * This overload solves the "dangling lambda" problem that arises when
     * writing `spawn(f())`: the temporary closure of `f` is destroyed as soon
     * as the call expression is evaluated, leaving the coroutine frame with a
     * dangling `this` pointer.
     *
     * By passing the callable WITHOUT invoking it, `invoke_owned` moves it
     * into a wrapper coroutine frame as a **value parameter**.  C++ guarantees
     * that function parameters are copied into the coroutine state, so the
     * callable stays alive for the entire duration of the spawned task.
     *
     * Usage:
     * @code
     * // BROKEN — lambda can dangle:
     * coro_scheduler().spawn([captured_data]() -> task<void> { ... }());
     *
     * // SAFE — pass the lambda itself, without the trailing ():
     * coro_scheduler().spawn([captured_data]() -> task<void> { ... });
     * @endcode
     *
     * @param fn Callable that returns task<void> (no arguments required)
     */
    template <typename Callable>
    requires std::invocable<Callable> && std::same_as<std::invoke_result_t<Callable>, task<void>>
             && (!std::same_as<std::decay_t<Callable>, task<void>>)
    void
    spawn(Callable fn) {
        // invoke_owned_ moves fn into its own coroutine frame (value param)
        spawn(invoke_owned_(std::move(fn)));
    }

    /**
     * @brief Schedule a coroutine for resumption
     *
     * Called by awaiters when their event fires. The coroutine
     * is added to the ready queue and will resume on next run_ready().
     *
     * Thread Safety (Finding 2.B.9): **must** be called from the thread
     * that owns this scheduler — typically the VirtualCore worker or the
     * I/O listener thread. Cross-thread wake-ups are NOT supported; use
     * the Actor mailbox instead if you need to signal a coroutine from a
     * different thread.
     *
     * @param handle The coroutine handle to resume
     */
    void
    schedule_resume(std::coroutine_handle<> handle) {
        if (!handle || handle.done())
            return;
        void *addr = handle.address();
        if (in_flight_.count(addr))
            return; // dedup: already queued
        in_flight_.insert(addr);
        ready_queue_.push_back({handle, false});
    }

    /**
     * @brief Enqueue a handle at the end of the ready queue without duplicate check
     *
     * Used by yield_awaiter to reschedule the current coroutine at the end of the
     * queue without allocating an intermediate coroutine. Call only when the handle
     * is not already in the queue (e.g. current running coroutine yielding).
     *
     * @param handle The coroutine handle to schedule
     */
    void
    enqueue_for_later(std::coroutine_handle<> handle) {
        if (!handle || handle.done())
            return;
        void *addr = handle.address();
        // Finding 2.B.1: without this dedup, calling `enqueue_for_later`
        // twice for the same handle (e.g. a `yield` loop that re-yields)
        // inserts one entry in `in_flight_` (set is idempotent) but pushes
        // **two** queue nodes — the coroutine would be resumed twice while
        // the first resume is still on the stack, which is UB per the
        // coroutine single-resume contract.
        if (in_flight_.count(addr))
            return;
        in_flight_.insert(addr);
        ready_queue_.push_back({handle, false});
    }

    /**
     * @brief Run ready coroutines
     *
     * Processes coroutines in the ready queue. If max_count is 0, drains the
     * queue; otherwise runs at most max_count coroutines (useful for tests that
     * expect "one step" semantics).
     * Only destroys handles that the scheduler owns (from spawn);
     * continuations own their inner frames and destroy them via the task.
     *
     * @param max_count Maximum number of coroutines to run; 0 = no limit
     * @return Number of coroutines executed
     */
    std::size_t
    run_ready(std::size_t max_count = 0) {
        // Finding 2.D.1: `run_ready()` must never be called re-entrantly —
        // from inside a coroutine body, from an awaiter's `await_suspend`,
        // or transitively via `listener::run()` invoked by user code that
        // is itself executing under a previous `run_ready()` frame. Doing
        // so would:
        //   1. Double-resume a handle that the outer frame already popped
        //      (since `in_flight_.erase` has already happened for it).
        //   2. Destroy handles while the outer frame still holds a local
        //      reference to them (`handle` on the stack).
        //   3. Break `max_count`-based fairness accounting.
        //
        // We guard the invariant with a per-scheduler boolean rather than a
        // thread_local so that the diagnostic is scoped to the actual
        // misuse — this also plays nicely with tests that create multiple
        // schedulers on one thread. In release we stay silent and simply
        // return 0 to stop the cascade, which is strictly safer than
        // marching on.
        if (in_run_ready_) {
#ifndef NDEBUG
            assert(!in_run_ready_
                   && "run_ready() called re-entrantly — "
                      "likely from run_sync() / run_for() invoked inside a "
                      "coroutine or actor handler. That is forbidden (see "
                      "qb-io coroutine invariants).");
#endif
            return 0;
        }
        struct ReentrancyGuard {
            bool &flag;
            explicit ReentrancyGuard(bool &f) noexcept
                : flag(f) {
                flag = true;
            }
            ~ReentrancyGuard() noexcept {
                flag = false;
            }
        } guard{in_run_ready_};

        std::size_t count = 0;

        while (!ready_queue_.empty()) {
            ready_item item = ready_queue_.front();
            ready_queue_.pop_front();

            if (item.handle)
                in_flight_.erase(item.handle.address());

            std::coroutine_handle<> handle = item.handle;

            if (handle && !handle.done()) {
                QB_SCHED_TRACE("run_ready resume handle=%p owned=%d", (void *) handle.address(), (int) item.owned);
                handle.resume();
                ++count;
                // Do NOT destroy completed frames here. A spawned coroutine
                // typically reaches final_suspend via SYMMETRIC TRANSFER from an
                // awaited inner task — its own handle is never re-examined by
                // this loop, so an inline destroy would miss it (frame leak).
                // Instead a detached frame defers its own destruction at
                // final_suspend (task<T>::final_awaiter) into frames_to_destroy_,
                // which is drained below. Continuation/owned-by-task<T> frames
                // are freed by their owning task object, never here.
            }

            if (max_count != 0 && count >= max_count) {
                break;
            }
        }

        // Drain frames whose detached (spawned) coroutines reached final_suspend
        // during this run. Each is suspended at final_suspend (not running), so
        // destroying it here is safe and frees the frame exactly once.
        while (!frames_to_destroy_.empty()) {
            auto h = frames_to_destroy_.back();
            frames_to_destroy_.pop_back();
            if (h) {
                owned_frames_.erase(h.address());
                h.destroy();
            }
        }

        return count;
    }

    /**
     * @brief Check if there are ready coroutines
     * @return true if the ready queue is not empty
     */
    [[nodiscard]] bool
    has_ready() const {
        return !ready_queue_.empty();
    }

    /**
     * @brief Check whether this scheduler is currently draining ready coroutines.
     *
     * Used by sync bridge helpers (`run_sync`, `run_for`) to reject nested event-loop
     * pumping from inside a coroutine/body already executing under `run_ready()`.
     */
    [[nodiscard]] bool
    is_draining_ready() const noexcept {
        return in_run_ready_;
    }

    /**
     * @brief Get the number of pending coroutines.
     * @return Exact size of the ready queue (O(1), mono-thread deque).
     */
    [[nodiscard]] std::size_t
    pending_count() const noexcept {
        return ready_queue_.size();
    }

    /**
     * @brief Get the current thread's scheduler
     * @return Reference to the thread-local scheduler
     *
     * Creates the scheduler on first access if it doesn't exist. Used as fallback
     * when no listener has set one. NOTE: Fallback uses new and is never deleted
     * (leak until thread exit). Prefer listener::coro_scheduler() so the listener owns it.
     */
    static CoroutineScheduler &
    current() {
        if (!current_) {
            current_ = new CoroutineScheduler();
        }
        return *current_;
    }

    /**
     * @brief Get pointer to current scheduler
     * @return Pointer to thread-local scheduler, or nullptr if not created
     */
    [[nodiscard]] static CoroutineScheduler *
    current_ptr() noexcept {
        return current_;
    }

    /**
     * @brief Set the current thread's scheduler
     * @param scheduler The scheduler to use
     *
     * Used by listener to set its internal scheduler as current.
     */
    static void
    set_current(CoroutineScheduler *scheduler) noexcept {
        current_ = scheduler;
    }

    /**
     * @brief Get the number of active coroutines tracked by this scheduler
     * @return Ready + suspended frames currently alive in this scheduler.
     *
     * Finding 2.B.6 — semantics:
     *   `active_count()` counts *ready* frames (pending in the ready queue,
     *   equivalent to `in_flight_.size()`) and *suspended* frames (parked
     *   on an awaiter). Previously it returned `in_flight_.size()
     *   + ready_queue_.size()`, which double-counts (every ready frame is
     *   in both containers) and silently excluded every coroutine sleeping
     *   on a watcher — defeating the purpose of the accessor for drain /
     *   shutdown loops.
     */
    [[nodiscard]] std::size_t
    active_count() const {
        return ready_queue_.size() + suspended_coroutines_.size();
    }

    /**
     * @brief Register a coroutine as suspended (waiting on I/O/timer)
     *
     * Called by awaiters when they suspend a coroutine. The coroutine
     * will be tracked until it is resumed or the scheduler is destroyed.
     *
     * @param handle The coroutine handle that is now suspended
     */
    void
    register_suspended(std::coroutine_handle<> handle) {
        if (!handle)
            return;
        suspended_coroutines_.insert(handle.address());
        QB_SCHED_TRACE("register_suspended handle=%p total_suspended=%zu", (void *) handle.address(), suspended_coroutines_.size());
    }

    /**
     * @brief Unregister a suspended coroutine (it was resumed or cancelled)
     *
     * Called by awaiters when they resume a coroutine or when the operation
     * completes/cancels.
     *
     * @param handle The coroutine handle that is no longer suspended
     */
    void
    unregister_suspended(std::coroutine_handle<> handle) {
        if (!handle)
            return;
        suspended_coroutines_.erase(handle.address());
        QB_SCHED_TRACE("unregister_suspended handle=%p remaining_suspended=%zu", (void *) handle.address(), suspended_coroutines_.size());
    }

    /**
     * @brief Destroy all suspended coroutines (stops their watchers via awaiter destructors).
     *
     * Call this before reset/destroy when the event loop is still valid (e.g. test TearDown)
     * so that no watchers remain active for the next test. Must not be used in ~CoroutineScheduler()
     * at process exit (loop may be tearing down); use only when explicitly resetting the scheduler.
     */
    void
    destroy_all_suspended() {
        std::vector<void *> to_destroy(suspended_coroutines_.begin(), suspended_coroutines_.end());
        suspended_coroutines_.clear();
        for (void *addr : to_destroy) {
            owned_frames_.erase(addr); // destroyed here — drop the ownership record
            auto handle = std::coroutine_handle<>::from_address(addr);
            if (handle && !handle.done()) {
                QB_SCHED_TRACE("destroy_all_suspended destroying handle=%p", (void *) addr);
                handle.destroy();
            }
        }
    }

    /**
     * @brief Queue a completed detached (spawned) frame for destruction.
     *
     * Called from task<T>::final_suspend when a spawned coroutine finishes with
     * no continuation to resume. The frame is suspended at final_suspend (it
     * cannot destroy itself), so run_ready() destroys it on the current drain.
     */
    void
    defer_destroy(std::coroutine_handle<> h) {
        if (h)
            frames_to_destroy_.push_back(h);
    }

private:
    /**
     * @brief Wrapper coroutine that owns the callable in its frame.
     *
     * Since fn is a **value parameter**, C++ copies/moves it into the
     * coroutine state at construction — the callable stays alive for the
     * entire duration of the spawned coroutine regardless of the original
     * lambda's lifetime.
     */
    template <typename F>
    static task<void>
    invoke_owned_(F fn) {
        co_await fn();
    }

    // Ready queue item: (handle, owned)
    // owned=true for handles from spawn() - scheduler must destroy when done
    // owned=false for continuations - they destroy themselves
    struct ready_item {
        std::coroutine_handle<> handle;
        bool                    owned;
    };

    // Finding 2.B.10: plain std::deque — mono-thread access, no atomics,
    // no mutex. std::deque allocates a block of nodes per chunk (~512 bytes
    // on libc++/libstdc++) which amortises allocation cost vs. the previous
    // per-node MPSC queue.
    std::deque<ready_item> ready_queue_;

    // Deduplication set: prevents double-scheduling of the same handle.
    // Accessed only from the VirtualCore thread — no mutex needed.
    std::unordered_set<void *> in_flight_;

    // Frames the scheduler OWNS and must destroy when they complete (those
    // handed over by spawn(), which detaches the task<T>). The per-ready_item
    // `owned` flag is NOT reliable for this: schedule_resume()/enqueue_for_later()
    // re-queue a resumed handle with owned=false, so a spawned coroutine that
    // suspends once and then completes would otherwise never be destroyed —
    // a frame leak on every spawn that awaits. This set is the authoritative
    // ownership record; run_ready() destroys a completed handle iff it is here.
    std::unordered_set<void *> owned_frames_;

    // Handles currently suspended (waiting on I/O or timers).
    // Accessed only from the VirtualCore thread — no mutex needed.
    std::unordered_set<void *> suspended_coroutines_;

    // Detached (spawned) frames that reached final_suspend and must be destroyed
    // by the scheduler on the current run_ready() drain. A completing detached
    // coroutine cannot destroy its own frame from inside final_suspend (it is
    // suspended in it), so it queues its handle here instead.
    std::vector<std::coroutine_handle<>> frames_to_destroy_;

    // Finding 2.D.1: re-entrancy guard for run_ready(). Set to true for the
    // duration of a `run_ready` call; the body of run_ready() checks this
    // up-front to refuse a nested invocation.
    bool in_run_ready_{false};

    // Reference to event loop
    ev::loop_ref loop_;

    // Thread-local current scheduler (defined in qb-io, e.g. io.cpp — one TU only)
    static thread_local CoroutineScheduler *current_;
};

// Global function for awaiters to get current scheduler
[[nodiscard]] inline CoroutineScheduler *
current_scheduler_ptr() noexcept {
    return CoroutineScheduler::current_ptr();
}

/**
 * @brief Schedule a coroutine via the current scheduler
 *
 * Helper function used by final_awaiter to schedule continuations, by
 * `shared_task`'s state flush, by channels / sync primitives / timers, etc.
 *
 * Precondition: a thread-local `CoroutineScheduler` must have been
 * established on this thread (this is done automatically for every
 * `qb::io::async::listener` when it is created). Calling from a thread
 * without a listener is a programming error — we `QB_ASSERT` in debug and
 * fall back to a silent no-op in release to avoid taking down the process,
 * but this means any waiter queued in that state will **never** be
 * resumed. Finding 2.A.2 documents this invariant.
 *
 * @param handle The coroutine handle to schedule
 */
inline void
schedule_via_current(std::coroutine_handle<> handle) noexcept {
    auto *sched = CoroutineScheduler::current_ptr();
#ifndef NDEBUG
    // Finding 2.A.2: fail loudly in debug so misconfigured test harnesses
    // or incorrect thread affinity bugs surface immediately rather than
    // manifesting as a test hang. Only guard when a real handle is queued.
    assert(sched
           && "schedule_via_current called without a TLS scheduler — "
              "did you forget to create a qb::io::async::listener on "
              "this thread?");
#endif
    if (sched) {
        sched->schedule_resume(handle);
    }
}

/**
 * @brief Enqueue a handle at the end of the current scheduler's ready queue
 *
 * Used by yield_awaiter to reschedule without allocating an intermediate coroutine.
 * The handle must not already be in the queue (e.g. current coroutine yielding).
 */
inline void
enqueue_for_later_via_current(std::coroutine_handle<> handle) noexcept {
    if (auto *sched = CoroutineScheduler::current_ptr()) {
        sched->enqueue_for_later(handle);
    }
}

/**
 * @brief Hand a completed detached (spawned) coroutine frame to the current
 *        scheduler for destruction. Declared in task.h and called from
 *        task<T>::final_suspend (which cannot deref the incomplete scheduler
 *        type itself). Defined here so the call resolves once scheduler.h is
 *        included (always the case via coroutine.h).
 */
inline void
defer_frame_destruction(std::coroutine_handle<> handle) noexcept {
    if (auto *sched = CoroutineScheduler::current_ptr()) {
        sched->defer_destroy(handle);
    }
}

/**
 * @brief Implementation of CoroutineScheduler::spawn
 *
 * The coroutine is added to the ready queue and will be resumed
 * on the next run_ready() call. The task's handle is transferred
 * to the scheduler.
 */
inline void
CoroutineScheduler::spawn(task<void> &&t) {
    auto handle = t.detach();
    if (!handle)
        return;

    // Set this scheduler as the coroutine's scheduler
    handle.promise().scheduler_ = this;

    owned_frames_.insert(handle.address());
    in_flight_.insert(handle.address());
    ready_queue_.push_back({handle, true});
    QB_SCHED_TRACE("spawn handle=%p in_flight=%zu", (void *) handle.address(), in_flight_.size());
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SCHEDULER_H
