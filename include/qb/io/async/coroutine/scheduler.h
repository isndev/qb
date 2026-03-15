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
#include <queue>
#include <unordered_set>
#include <ev/ev++.h>
#include <atomic>
#include <mutex>

// Forward declarations to avoid circular includes
namespace qb::io::async {
    template<typename T> class task;
}

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
 * - This class is NOT thread-safe by design
 * - All methods must be called from the thread that owns the scheduler
 * - The only exception is schedule_resume() which can be called from libev callbacks
 *   (which run on the same thread as the event loop)
 * - Use separate scheduler instances for each thread
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
     * Cleans up any pending coroutines in the ready queue.
     * Suspended coroutines (waiting on I/O or timers) are not affected
     * as they are owned by their respective awaiters.
     */
    ~CoroutineScheduler() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!ready_queue_.empty()) {
            ready_item item = ready_queue_.front();
            ready_queue_.pop();
            if (item.handle && !item.handle.done()) {
                item.handle.destroy();
            }
        }
        in_flight_.clear();
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
        if (!handle || handle.done()) {
            return;
        }

        std::lock_guard<std::mutex> lock(queue_mutex_);

        // Check if already scheduled to avoid duplicates
        void* addr = handle.address();
        if (in_flight_.count(addr)) {
            return;
        }

        in_flight_.insert(addr);
        ready_queue_.push({handle, false});  // not owned: continuation will destroy
    }

    /**
     * @brief Run all ready coroutines
     *
     * Processes all coroutines currently in the ready queue.
     * Only destroys handles that the scheduler owns (from spawn);
     * continuations own their inner frames and destroy them via the task.
     *
     * @return Number of coroutines executed
     */
    std::size_t run_ready() {
        std::size_t count = 0;
        std::queue<ready_item> current_batch;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::swap(current_batch, ready_queue_);
            // Note: in_flight_ is cleared as we process items
        }

#ifdef QB_DEBUG_COROUTINES
        std::cerr << "[SCHEDULER] run_ready: processing " << current_batch.size() << " items\n";
#endif

        while (!current_batch.empty()) {
            ready_item item = current_batch.front();
            current_batch.pop();
            std::coroutine_handle<> handle = item.handle;
            bool owned = item.owned;

            // Remove from in_flight_ BEFORE processing to allow re-scheduling
            if (handle) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                in_flight_.erase(handle.address());
            }

            if (handle && !handle.done()) {
                // Resume the coroutine
#ifdef QB_DEBUG_COROUTINES
                std::cerr << "[SCHEDULER] Resuming handle=" << handle.address() 
                          << " owned=" << owned << "\n";
#endif
                handle.resume();
#ifdef QB_DEBUG_COROUTINES
                std::cerr << "[SCHEDULER] After resume, done=" << handle.done() << "\n";
#endif
                ++count;

                // Only destroy if we own it AND it's done
                if (owned && handle.done()) {
#ifdef QB_DEBUG_COROUTINES
                    std::cerr << "[SCHEDULER] Destroying owned handle=" << handle.address() << "\n";
#endif
                    handle.destroy();
                }
            } else if (owned && handle.done()) {
                // Handle was already done when we got it, destroy it
                // This can happen for coroutines with no suspension points
#ifdef QB_DEBUG_COROUTINES
                std::cerr << "[SCHEDULER] Destroying already-done handle=" << handle.address() << "\n";
#endif
                handle.destroy();
            }
        }

        return count;
    }

    /**
     * @brief Check if there are ready coroutines
     * @return true if the ready queue is not empty
     */
    [[nodiscard]] bool has_ready() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
        return !ready_queue_.empty();
    }

    /**
     * @brief Get the number of pending coroutines
     * @return Size of the ready queue
     */
    [[nodiscard]] std::size_t pending_count() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
        return ready_queue_.size();
    }

    /**
     * @brief Get the current thread's scheduler
     * @return Reference to the thread-local scheduler
     *
     * Creates the scheduler on first access if it doesn't exist.
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
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
        return in_flight_.size() + ready_queue_.size();
    }

private:
    // Ready queue item: (handle, owned)
    // owned=true for handles from spawn() - scheduler must destroy when done
    // owned=false for continuations - they destroy themselves
    struct ready_item {
        std::coroutine_handle<> handle;
        bool owned;
    };

    std::queue<ready_item> ready_queue_;
    mutable std::mutex queue_mutex_;  // Protects ready_queue_ and in_flight_

    // Track coroutines that are in-flight (scheduled but not yet run)
    // This prevents duplicate scheduling
    std::unordered_set<void*> in_flight_;

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
 * Defined here to avoid circular dependency with task.h
 *
 * @param handle The coroutine handle to schedule
 */
inline void schedule_via_current(std::coroutine_handle<> handle) noexcept {
    if (auto* sched = CoroutineScheduler::current_ptr()) {
        sched->schedule_resume(handle);
    }
    // If no scheduler exists, the handle won't be resumed
    // This is typically a bug - the scheduler should be initialized
#ifdef QB_DEBUG_COROUTINES
    else if (handle) {
        std::cerr << "[SCHEDULE] Warning: No scheduler available for handle "
                  << handle.address() << "\n";
    }
#endif
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SCHEDULER_H
