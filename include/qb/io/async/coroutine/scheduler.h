/**
 * @file qb/io/async/coroutine/scheduler.h
 * @brief Coroutine scheduler for libev integration
 *
 * Manages coroutine execution and lifecycle, tracks ready coroutines, and integrates
 * with the libev event loop for efficient suspend/resume operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <coroutine>
#include <unordered_set>
#include <vector>
#include <ev/ev++.h>
// No <mutex>: scheduler runs exclusively on one VirtualCore thread.
// All callers (libev callbacks, coroutine bodies, schedule_via_current) are
// on the same thread — sequential access, no locking needed.
// <atomic> kept only for #ifdef QB_DEBUG_COROUTINES next_id in task.h.
#include <qb/system/lockfree/mpsc_unbounded_queue.h>

/** Enable scheduler/listener lifecycle debug traces (destructor, register_suspended, clear, reset).
 *  Build with -DQB_DEBUG_CORO_LIFECYCLE=1 to trace teardown and suspended counts. */
#include <cstdio>
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif
#define QB_SCHED_TRACE(fmt, ...) std::fprintf(stderr, "[sched] " fmt "\n", ##__VA_ARGS__)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#elif defined(QB_DEBUG_CORO_LIFECYCLE) && QB_DEBUG_CORO_LIFECYCLE
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif
#define QB_SCHED_TRACE(fmt, ...) std::fprintf(stderr, "[sched] " fmt "\n", ##__VA_ARGS__)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
#define QB_SCHED_TRACE(fmt, ...) ((void)0)
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
 * Thread Safety:
 * ==============
 * - Designed for single-thread: one thread runs the event loop and run_ready().
 * - schedule_resume() / enqueue_for_later() can be called from libev callbacks (same thread)
 *   or from inside a resumed coroutine (same thread, same call stack). The queue is then
 *   accessed from multiple points on the same stack (run_ready() loop vs. inside resume()).
 * - ready_queue_ is a lock-free MPSC queue (no mutex on push/pop of the queue).
 * - in_flight_ and suspended_coroutines_ are plain unordered_sets: no mutex needed
 *   because all accesses are on the same thread (single-thread cooperative model).
 * - Push path: in_flight_ check/insert, then lock-free queue push.
 * - Pop path: lock-free queue pop, then in_flight_ erase.
 * - run_ready() must not be called re-entrantly (do not call from inside a coroutine).
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
        : loop_(loop) {
    }

    /**
     * @brief Destructor
     *
     * Drains and destroys all coroutines in the ready queue only. Coroutines
     * that are suspended (waiting on I/O or timers) are NOT destroyed, because
     * their libev watchers are still active; destroying those handles would
     * run awaiter destructors that call ev_*_stop(), which can cause use-after-free
     * or double-free if the loop is being torn down or if callbacks are in flight.
     *
     * For clean process exit, the application should stop the event loop before
     * destroying the scheduler (e.g. listener::break_one() then run to drain).
     * Suspended coroutine frames are left alive at process exit (OS reclaims memory).
     */
    ~CoroutineScheduler() {
        QB_SCHED_TRACE("~CoroutineScheduler() begin this=%p", (void*)this);
        std::size_t ready_drained = 0;
        ready_item item;
        while (ready_queue_.pop(item)) {
            if (item.handle && !item.handle.done()) {
                QB_SCHED_TRACE("  destroy ready handle=%p owned=%d", (void*)item.handle.address(), item.owned);
                item.handle.destroy();
            }
            ++ready_drained;
        }
        QB_SCHED_TRACE("  drained ready_queue: %zu items", ready_drained);
        (void)ready_drained;
        QB_SCHED_TRACE("  clearing suspended_coroutines_ (count=%zu), NOT destroying handles", suspended_coroutines_.size());
        suspended_coroutines_.clear();
        in_flight_.clear();
        QB_SCHED_TRACE("~CoroutineScheduler() end this=%p", (void*)this);
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
    void spawn(task<void>&& t);

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
    requires std::invocable<Callable>
          && std::same_as<std::invoke_result_t<Callable>, task<void>>
          && (!std::same_as<std::decay_t<Callable>, task<void>>)
    void spawn(Callable fn) {
        // invoke_owned_ moves fn into its own coroutine frame (value param)
        spawn(invoke_owned_(std::move(fn)));
    }

    /**
     * @brief Schedule a coroutine for resumption
     *
     * Called by awaiters when their event fires. The coroutine
     * is added to the ready queue and will resume on next run_ready().
     *
     * Thread Safety: This method is thread-safe and can be called from
     * libev callbacks which run on the same thread as the event loop.
     *
     * @param handle The coroutine handle to resume
     */
    void schedule_resume(std::coroutine_handle<> handle) {
        if (!handle || handle.done()) return;
        void* addr = handle.address();
        if (in_flight_.count(addr)) return;  // dedup: already queued
        in_flight_.insert(addr);
        ready_queue_.push({handle, false});
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
    void enqueue_for_later(std::coroutine_handle<> handle) {
        if (!handle || handle.done()) return;
        in_flight_.insert(handle.address());
        ready_queue_.push({handle, false});
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
    std::size_t run_ready(std::size_t max_count = 0) {
        std::size_t count = 0;
        ready_item item;

        while (ready_queue_.pop(item)) {
            if (item.handle)
                in_flight_.erase(item.handle.address());

            std::coroutine_handle<> handle = item.handle;
            bool owned = item.owned;

            if (handle && !handle.done()) {
                QB_SCHED_TRACE("run_ready resume handle=%p owned=%d", (void*)handle.address(), owned);
                handle.resume();
                ++count;

                if (owned && handle.done()) {
                    QB_SCHED_TRACE("run_ready destroy completed handle=%p", (void*)handle.address());
                    handle.destroy();
                }
            } else if (owned && handle && handle.done()) {
                QB_SCHED_TRACE("run_ready destroy done handle=%p", (void*)handle.address());
                handle.destroy();
            }

            if (max_count != 0 && count >= max_count) {
                break;
            }
        }

        return count;
    }

    /**
     * @brief Check if there are ready coroutines
     * @return true if the ready queue is not empty
     */
    [[nodiscard]] bool has_ready() const {
        return !ready_queue_.empty();
    }

    /**
     * @brief Get the number of pending coroutines (approximate; can change immediately).
     * @return Approximate size of the ready queue
     */
    [[nodiscard]] std::size_t pending_count() const {
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
    static CoroutineScheduler& current() {
        if (!current_) {
            current_ = new CoroutineScheduler();
        }
        return *current_;
    }

    /**
     * @brief Get pointer to current scheduler
     * @return Pointer to thread-local scheduler, or nullptr if not created
     */
    [[nodiscard]] static CoroutineScheduler* current_ptr() noexcept {
        return current_;
    }

    /**
     * @brief Set the current thread's scheduler
     * @param scheduler The scheduler to use
     *
     * Used by listener to set its internal scheduler as current.
     */
    static void set_current(CoroutineScheduler* scheduler) noexcept {
        current_ = scheduler;
    }

    /**
     * @brief Get the number of active coroutines tracked by this scheduler
     * @return Number of coroutines currently in-flight or ready
     */
    [[nodiscard]] std::size_t active_count() const {
        return in_flight_.size() + ready_queue_.size();
    }

    /**
     * @brief Register a coroutine as suspended (waiting on I/O/timer)
     *
     * Called by awaiters when they suspend a coroutine. The coroutine
     * will be tracked until it is resumed or the scheduler is destroyed.
     *
     * @param handle The coroutine handle that is now suspended
     */
    void register_suspended(std::coroutine_handle<> handle) {
        if (!handle) return;
        suspended_coroutines_.insert(handle.address());
        QB_SCHED_TRACE("register_suspended handle=%p total_suspended=%zu", (void*)handle.address(), suspended_coroutines_.size());
    }

    /**
     * @brief Unregister a suspended coroutine (it was resumed or cancelled)
     *
     * Called by awaiters when they resume a coroutine or when the operation
     * completes/cancels.
     *
     * @param handle The coroutine handle that is no longer suspended
     */
    void unregister_suspended(std::coroutine_handle<> handle) {
        if (!handle) return;
        suspended_coroutines_.erase(handle.address());
        QB_SCHED_TRACE("unregister_suspended handle=%p remaining_suspended=%zu", (void*)handle.address(), suspended_coroutines_.size());
    }

    /**
     * @brief Destroy all suspended coroutines (stops their watchers via awaiter destructors).
     *
     * Call this before reset/destroy when the event loop is still valid (e.g. test TearDown)
     * so that no watchers remain active for the next test. Must not be used in ~CoroutineScheduler()
     * at process exit (loop may be tearing down); use only when explicitly resetting the scheduler.
     */
    void destroy_all_suspended() {
        std::vector<void*> to_destroy(suspended_coroutines_.begin(), suspended_coroutines_.end());
        suspended_coroutines_.clear();
        for (void* addr : to_destroy) {
            auto handle = std::coroutine_handle<>::from_address(addr);
            if (handle && !handle.done()) {
                QB_SCHED_TRACE("destroy_all_suspended destroying handle=%p", (void*)addr);
                handle.destroy();
            }
        }
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
    static task<void> invoke_owned_(F fn) {
        co_await fn();
    }

    // Ready queue item: (handle, owned)
    // owned=true for handles from spawn() - scheduler must destroy when done
    // owned=false for continuations - they destroy themselves
    struct ready_item {
        std::coroutine_handle<> handle;
        bool owned;
    };

    // Lock-free queue: push from coroutine bodies / libev callbacks (all same thread),
    // pop from run_ready() — no mutex needed, but MPSC is correct for the interface.
    qb::lockfree::mpsc_unbounded_queue<ready_item> ready_queue_;

    // Deduplication set: prevents double-scheduling of the same handle.
    // Accessed only from the VirtualCore thread — no mutex needed.
    std::unordered_set<void*> in_flight_;

    // Handles currently suspended (waiting on I/O or timers).
    // Accessed only from the VirtualCore thread — no mutex needed.
    std::unordered_set<void*> suspended_coroutines_;

    // Reference to event loop
    ev::loop_ref loop_;

    // Thread-local current scheduler
    static thread_local CoroutineScheduler* current_;
};

// Initialize thread-local static (inline for C++17)
inline thread_local CoroutineScheduler* CoroutineScheduler::current_ = nullptr;

// Global function for awaiters to get current scheduler
[[nodiscard]] inline CoroutineScheduler* current_scheduler_ptr() noexcept {
    return CoroutineScheduler::current_ptr();
}

/**
 * @brief Schedule a coroutine via the current scheduler
 *
 * Helper function used by final_awaiter to schedule continuations.
 *
 * @param handle The coroutine handle to schedule
 */
inline void schedule_via_current(std::coroutine_handle<> handle) noexcept {
    if (auto* sched = CoroutineScheduler::current_ptr()) {
        sched->schedule_resume(handle);
    }
}

/**
 * @brief Enqueue a handle at the end of the current scheduler's ready queue
 *
 * Used by yield_awaiter to reschedule without allocating an intermediate coroutine.
 * The handle must not already be in the queue (e.g. current coroutine yielding).
 */
inline void enqueue_for_later_via_current(std::coroutine_handle<> handle) noexcept {
    if (auto* sched = CoroutineScheduler::current_ptr()) {
        sched->enqueue_for_later(handle);
    }
}

/**
 * @brief Implementation of CoroutineScheduler::spawn
 *
 * The coroutine is added to the ready queue and will be resumed
 * on the next run_ready() call. The task's handle is transferred
 * to the scheduler.
 */
inline void CoroutineScheduler::spawn(task<void>&& t) {
    auto handle = t.detach();
    if (!handle) return;

    // Set this scheduler as the coroutine's scheduler
    handle.promise().scheduler_ = this;

    in_flight_.insert(handle.address());
    ready_queue_.push({handle, true});
    QB_SCHED_TRACE("spawn handle=%p in_flight=%zu", (void*)handle.address(), in_flight_.size());
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SCHEDULER_H
