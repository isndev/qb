/**
 * @file qb/io/async/coroutine/awaiter.h
 * @brief Coroutine awaiters for libev integration
 *
 * This file defines awaiter types that integrate C++20 coroutines
 * with the libev event loop. Awaiters allow coroutines to suspend
 * on I/O operations and resume when those operations complete.
 *
 * USAGE GUIDELINES:
 * ================
 *
 * Timer Awaiter (sleep):
 * @code
 * task<void> delayed_operation() {
 *     co_await sleep(std::chrono::milliseconds(100));
 *     // Resumes after 100ms
 * }
 * @endcode
 *
 * Socket Awaiter (I/O):
 * @code
 * task<void> read_data(int fd) {
 *     co_await wait_readable(fd);
 *     // fd now has data available
 *     char buffer[1024];
 *     read(fd, buffer, sizeof(buffer));
 * }
 * @endcode
 *
 * CRITICAL: Awaiter Lifetime
 * ===========================
 * - Awaiters must remain alive until await_resume() is called
 * - Never create temporary awaiters that go out of scope before resumption
 * - The scheduler manages awaiter lifetime through the ready queue
 *
 * THREAD SAFETY:
 * ==============
 * - Awaiters are used from a single OS thread per `listener::current` (VirtualCore worker).
 *   `on_event_ready()` runs on that same thread when libev invokes the watcher callback.
 * - Do not share an awaiter or its watcher across threads; use one listener per thread.
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

#ifndef QB_IO_ASYNC_COROUTINE_AWAITER_H
#define QB_IO_ASYNC_COROUTINE_AWAITER_H

#include <coroutine>
#include <chrono>
#include <functional>
// No <atomic>: libev callbacks fire on the same thread as the coroutines
// (VirtualCore thread). Plain bools are sufficient for single-thread cooperative.
#include <memory>
#include <ev/ev++.h>
#include <qb/system/time.h> // qb::duration, qb::detail::to_ev_seconds

// scheduler.h must be included before task.h to get schedule_via_current
#include "scheduler.h"

namespace qb::io::async {

/**
 * @brief Base class for libev-based awaiters
 *
 * Provides common functionality for coroutine awaiters that integrate
 * with the libev event loop. Derived classes implement specific
 * event types (timer, I/O, etc.).
 *
 * This implementation uses a safe callback pattern where the libev watcher
 * is always stopped in the await_resume() or destructor, preventing
 * use-after-free issues.
 *
 * @ingroup Coroutine
 */
struct awaiter_base {
    /**
     * @brief Coroutine handle to resume
     */
    std::coroutine_handle<> handle_;

    /**
     * @brief Flag indicating if result is ready
     * Plain bool: libev callback and coroutine run on the same thread.
     */
    bool ready_{false};

    /**
     * @brief Guard against double-resume (e.g. watcher fires after await_resume).
     * Plain bool: no concurrent access in single-thread cooperative model.
     */
    bool resumed_{false};

    /**
     * @brief Pointer to the scheduler
     */
    CoroutineScheduler *scheduler_ = nullptr;

    /**
     * @brief Default constructor
     */
    awaiter_base() noexcept = default;

    /**
     * @brief Construct with handle
     * @param h The coroutine handle
     */
    explicit awaiter_base(std::coroutine_handle<> h) noexcept
        : handle_(h) {}

    // Non-copyable, non-movable
    awaiter_base(const awaiter_base &)            = delete;
    awaiter_base &operator=(const awaiter_base &) = delete;
    awaiter_base(awaiter_base &&)                 = delete;
    awaiter_base &operator=(awaiter_base &&)      = delete;

    /**
     * @brief Check if awaiter is ready
     * @return true if the operation completed synchronously
     */
    [[nodiscard]] bool
    await_ready() const noexcept {
        return ready_;
    }

    /**
     * @brief Suspend and register with libev
     * @param h The coroutine handle to resume later
     *
     * Derived classes must implement this to register with libev.
     * The implementation must store the handle and start the watcher.
     */
    virtual void await_suspend(std::coroutine_handle<> h) = 0;

    /**
     * @brief Resume the coroutine
     *
     * Called when the operation completes. Stops the watcher.
     */
    virtual void
    await_resume() {
        resumed_ = true;
    }

    /**
     * @brief Called by libev when event fires
     *
     * Schedules the coroutine for resumption via the scheduler.
     * Must run on the thread that owns `listener::current` (libev callback thread).
     */
    void
    on_event_ready() noexcept {
        ready_ = true;
        if (resumed_)
            return; // watcher fired after await_resume — ignore

        // Unregister from suspended set before resuming
        if (scheduler_ && handle_) {
            scheduler_->unregister_suspended(handle_);
        }

#ifdef QB_DEBUG_COROUTINES
        std::cerr << "[AWAITER] on_event_ready: scheduler=" << scheduler_ << " handle=" << handle_.address() << "\n";
#endif

        if (scheduler_ && handle_) {
            scheduler_->schedule_resume(handle_);
        }
#ifdef QB_DEBUG_COROUTINES
        else {
            std::cerr << "[AWAITER] on_event_ready: NOT scheduling (scheduler=" << scheduler_ << " handle=" << handle_.address() << ")\n";
        }
#endif
    }

    /**
     * @brief Register the coroutine as suspended
     *
     * Called by derived classes in await_suspend to track this coroutine
     * as waiting for an event.
     */
    void
    register_suspended() noexcept {
        if (scheduler_ && handle_) {
            scheduler_->register_suspended(handle_);
        }
    }

    /**
     * @brief Remove the coroutine from the suspended set if it is still tracked.
     *
     * Safe to call multiple times: the scheduler-side erase is idempotent.
     * This is needed both on normal resume and on awaiter destruction paths
     * where the coroutine frame goes away before the event fires.
     */
    void
    unregister_suspended() noexcept {
        if (scheduler_ && handle_) {
            scheduler_->unregister_suspended(handle_);
        }
    }

    /**
     * @brief Fully unschedule this coroutine on frame teardown (stronger than unregister).
     *
     * Once on_event_ready() has fired, the frame has moved OUT of the suspended set and INTO
     * the ready queue + in-flight set — it is queued for resume. If the frame is then destroyed
     * before run_ready() pops it (e.g. a losing `when_any` branch reclaimed in the same drain,
     * or a cancelled `cancellable_operation`), `unregister_suspended()` alone leaves a DANGLING
     * handle in those queues for the next drain to resume — a use-after-free. `forget()` scrubs
     * all three sets (suspended + in-flight + ready queue), so destroying a frame is always safe
     * regardless of whether its watcher had already fired this tick. Destructors must call this
     * (not just unregister_suspended) so the queued-leaf-then-destroyed window cannot UAF.
     */
    void
    unschedule() noexcept {
        if (scheduler_ && handle_) {
            scheduler_->forget(handle_);
        }
    }

    /**
     * @brief Virtual destructor
     */
    virtual ~awaiter_base() = default;
};

/**
 * @brief Timer-based awaiter
 *
 * Suspends the coroutine until a specified duration elapses.
 * Uses libev's ev_timer watcher internally.
 *
 * This implementation is exception-safe and prevents use-after-free
 * by stopping the watcher in await_resume() and the destructor.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> delay() {
 *     co_await qb::io::async::timer_awaiter{std::chrono::seconds(1)};
 * }
 * @endcode
 *
 * @ingroup Coroutine
 */
struct timer_awaiter : awaiter_base {
    /**
     * @brief libev timer watcher
     */
    ev_timer watcher_{};

    /**
     * @brief Reference to the event loop
     */
    ev::loop_ref loop_;

    /**
     * @brief Zero/negative sleep is a cooperative yield, not a real timer.
     *
     * On Windows, a 1ms libev timer is often quantized to the system timer
     * resolution (~15.6ms). For `sleep(0ms)` callers expect "yield and resume
     * on the next scheduler turn", not "arm a kernel timer". We therefore
     * bypass libev entirely in that case and simply enqueue the coroutine at
     * the back of the ready queue.
     */
    bool yield_only_ = false;

    /**
     * @brief Flag to track if watcher was started
     */
    bool started_ = false;

    /**
     * @brief Construct with duration
     * @param duration The time to wait
     * @param loop The event loop (defaults to current)
     */
    timer_awaiter(qb::duration duration, ev::loop_ref loop = ev::get_default_loop())
        : loop_(loop)
        , yield_only_(duration.count() <= 0) {
        if (!yield_only_) {
            double after = qb::detail::to_ev_seconds(duration);
            ev_timer_init(&watcher_, timer_callback, after, 0.0);
            watcher_.data = this;
        }
    }

    /**
     * @brief Suspend and start timer
     * @param h The coroutine handle
     *
     * Stores the handle and starts the libev timer watcher.
     * The scheduler is obtained from the current thread.
     */
    void
    await_suspend(std::coroutine_handle<> h) override {
        handle_    = h;
        scheduler_ = CoroutineScheduler::current_ptr();
        if (!scheduler_) {
            // Fallback: create/get the current scheduler
            scheduler_ = &CoroutineScheduler::current();
        }
        if (yield_only_) {
            enqueue_for_later_via_current(h);
            return;
        }
        register_suspended(); // Track this coroutine as suspended
        // Finding 2.C.3: libev caches the current monotonic time in `mn_now`
        // at loop iteration boundaries. If the worker thread has been out of
        // the loop for a while (e.g. a blocking `std::this_thread::sleep_for`
        // from user code, or a synchronous I/O client call), that cache is
        // stale — a newly started `ev_timer` computes its expiration relative
        // to the stale time and may fire immediately. We fix that by forcing
        // a time-cache refresh before starting the watcher, mirroring the
        // same fix we landed in `async::callback` (io.h).
        ev_now_update(static_cast<struct ev_loop *>(loop_));
        ev_timer_start(loop_, &watcher_);
        started_ = true;
    }

    /**
     * @brief Stop timer on resume
     *
     * Stops the watcher to prevent it from firing after resumption.
     * Also marks the awaiter as resumed and unregisters from suspended set.
     */
    void
    await_resume() override {
        if (yield_only_) {
            awaiter_base::await_resume();
            return;
        }
        // Always stop when armed — see the destructor's note on the one-shot
        // auto-stop / still-pending window. `ev_timer_stop` clears any pending
        // event for this watcher first, then no-ops if already inactive.
        if (started_) {
            ev_timer_stop(loop_, &watcher_);
            started_ = false;
        }
        // Unregister from suspended set (in case on_event_ready wasn't called)
        unregister_suspended();
        awaiter_base::await_resume();
    }

    /**
     * @brief Destructor - ensures timer is stopped
     *
     * If the awaiter is destroyed before the timer fires (e.g., due to
     * exception or early return), the timer is stopped to prevent
     * use-after-free.
     */
    ~timer_awaiter() override {
        unschedule(); // full scrub: a fired-but-not-yet-resumed frame must leave ready_queue_/in_flight_ too
        if (yield_only_) {
            return;
        }
        // CRITICAL — do NOT gate this on `ev_is_active`. A one-shot ev_timer
        // (repeat == 0, which `sleep()` always uses) is auto-stopped by libev's
        // `timers_reify()` the instant it expires — BEFORE its callback is
        // invoked (ev.c: `else ev_timer_stop(w); /* nonrepeating */`). So in the
        // window between expiry and `ev_invoke_pending()` the watcher is
        // INACTIVE but still PENDING: it sits in libev's `pendings[]` array with
        // `w->data` pointing at THIS awaiter. If the coroutine frame holding this
        // awaiter is destroyed in that window (e.g. a same-tick cancellation tears
        // down the inner task before the scheduler drains the ready queue), an
        // `ev_is_active`-gated stop would be skipped, leaving the freed watcher in
        // `pendings[]`. `ev_invoke_pending()` would then call `timer_callback()`
        // on freed memory and `schedule_resume()` a destroyed handle — a
        // use-after-free that surfaces as a wild `handle.resume()` in run_ready().
        // `ev_timer_stop()` calls `clear_pending()` FIRST, unconditionally (it
        // evicts the watcher from `pendings[]`), then no-ops if already inactive —
        // so calling it whenever the timer was armed is both necessary and safe.
        if (started_) {
            ev_timer_stop(loop_, &watcher_);
            started_ = false;
        }
    }

    /**
     * @brief libev callback when timer fires
     *
     * Static callback invoked by libev. Schedules the coroutine for resumption.
     */
    static void
    timer_callback(struct ev_loop *, ev_timer *w, int) noexcept {
        auto *self = static_cast<timer_awaiter *>(w->data);
        if (self) {
#ifdef QB_DEBUG_COROUTINES
            std::cerr << "[TIMER] Timer fired for awaiter " << self << "\n";
#endif
            self->on_event_ready();
        }
    }
};

/**
 * @brief Socket I/O awaiter
 *
 * Suspends the coroutine until a socket is ready for reading/writing.
 * Uses libev's ev_io watcher internally.
 *
 * This implementation is exception-safe and prevents use-after-free
 * by stopping the watcher in await_resume() and the destructor.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> wait_for_data(int fd) {
 *     co_await qb::io::async::socket_awaiter{fd, EV_READ};
 *     // Socket is now readable
 * }
 * @endcode
 *
 * @ingroup Coroutine
 */
struct socket_awaiter : awaiter_base {
    /**
     * @brief File descriptor
     */
    int fd_ = -1;

    /**
     * @brief Events to wait for (EV_READ, EV_WRITE, or both)
     */
    int events_ = 0;

    /**
     * @brief libev I/O watcher
     */
    ev_io watcher_{};

    /**
     * @brief Reference to event loop
     */
    ev::loop_ref loop_;

    /**
     * @brief Flag to track if watcher was started
     */
    bool started_ = false;

    /**
     * @brief Construct with file descriptor and events
     * @param fd The file descriptor
     * @param events Events to wait for (EV_READ, EV_WRITE)
     * @param loop The event loop (defaults to current)
     */
    socket_awaiter(int fd, int events, ev::loop_ref loop = ev::get_default_loop())
        : fd_(fd)
        , events_(events)
        , loop_(loop) {
        ev_io_init(&watcher_, io_callback, fd, events);
        watcher_.data = this;
    }

#if defined(_WIN32)
    socket_awaiter(uintptr_t handle, int events, ev::loop_ref loop = ev::get_default_loop())
        : events_(events)
        , loop_(loop) {
        ev_io_init_sock(&watcher_, io_callback, handle, events);
        watcher_.data = this;
    }
#endif

    /**
     * @brief Suspend and start I/O watcher
     * @param h The coroutine handle
     *
     * Stores the handle and starts the libev I/O watcher.
     * The scheduler is obtained from the current thread.
     */
    void
    await_suspend(std::coroutine_handle<> h) override {
        handle_    = h;
        scheduler_ = CoroutineScheduler::current_ptr();
        if (!scheduler_) {
            scheduler_ = &CoroutineScheduler::current();
        }
        register_suspended(); // Track this coroutine as suspended
        ev_io_start(loop_, &watcher_);
        started_ = true;
    }

    /**
     * @brief Stop watcher on resume
     *
     * Stops the watcher to prevent it from firing after resumption.
     * Also marks the awaiter as resumed and unregisters from suspended set.
     */
    void
    await_resume() override {
        // Gate only on `started_`, never on `ev_is_active`: `ev_io_stop` clears
        // any pending event for this watcher first (so a fired-but-not-yet-drained
        // watcher cannot be invoked on a freed frame later), then no-ops if the
        // watcher is already inactive. Mirrors the timer_awaiter teardown note.
        if (started_) {
            ev_io_stop(loop_, &watcher_);
            started_ = false;
        }
        // Unregister from suspended set (in case on_event_ready wasn't called)
        unregister_suspended();
        awaiter_base::await_resume();
    }

    /**
     * @brief Destructor - ensures watcher is stopped
     *
     * If the awaiter is destroyed before I/O is ready (e.g., due to
     * exception or early return), the watcher is stopped to prevent
     * use-after-free.
     */
    ~socket_awaiter() override {
        unschedule(); // full scrub: a fired-but-not-yet-resumed frame must leave ready_queue_/in_flight_ too
        // Gate only on `started_` (not `ev_is_active`): `ev_io_stop` clears any
        // pending event for this watcher first, then no-ops if already inactive —
        // so a watcher that fired this tick but whose frame is being destroyed
        // before run_ready() drains it can never be invoked on freed memory.
        // See the timer_awaiter destructor for the full one-shot rationale.
        if (started_) {
            ev_io_stop(loop_, &watcher_);
            started_ = false;
        }
    }

    /**
     * @brief libev callback when I/O is ready
     *
     * Static callback invoked by libev. Schedules the coroutine for resumption.
     */
    static void
    io_callback(struct ev_loop *, ev_io *w, int revents) noexcept {
        auto *self = static_cast<socket_awaiter *>(w->data);
        if (self) {
            (void) revents; // Could check which event fired for logging
            self->on_event_ready();
        }
    }
};

/**
 * @brief Generic async operation awaiter
 *
 * Bridges existing callback-based APIs to coroutines.
 * The async operation is started in await_suspend, and the
 * callback resumes the coroutine.
 *
 * This implementation is safe for use with C++ lambdas and std::function.
 * The async operation is started with a callback that captures the awaiter
 * safely.
 *
 * Usage:
 * @code
 * qb::io::async::task<int> legacy_call() {
 *     auto awaiter = qb::io::async::async_awaiter<int>([](auto cb) {
 *         legacy_async_function([](int result) {
 *             cb(result);
 *         });
 *     });
 *     int result = co_await awaiter;
 *     co_return result;
 * }
 * @endcode
 *
 * @tparam ResultType The type of the async result
 * @ingroup Coroutine
 */
template <typename ResultType>
struct async_awaiter : awaiter_base {
    using callback_type = std::function<void(ResultType)>;

    /**
     * @brief Storage for result
     */
    std::optional<ResultType> result_;

    /**
     * @brief The async operation function
     */
    std::function<void(callback_type)> async_op_;

    /**
     * @brief Internal callback that wraps the user's callback
     */
    callback_type callback_;

    /**
     * @brief Validity flag: set false in destructor so callback no-ops if
     * the coroutine/awaiter was destroyed before the async op completed.
     * Plain bool (via shared_ptr): lifecycle events are sequential on one thread.
     */
    std::shared_ptr<bool> valid_;

    /**
     * @brief Construct with async operation
     * @param async_op Function that starts the async operation
     */
    explicit async_awaiter(std::function<void(callback_type)> async_op)
        : async_op_(std::move(async_op)) {}

    /**
     * @brief Suspend and start async operation
     * @param h The coroutine handle
     *
     * Callback uses valid_ to avoid use-after-free if the awaiter is
     * destroyed (e.g. exception) before the async operation completes.
     */
    void
    await_suspend(std::coroutine_handle<> h) override {
        handle_    = h;
        scheduler_ = CoroutineScheduler::current_ptr();
        if (!scheduler_) {
            scheduler_ = &CoroutineScheduler::current();
        }
        register_suspended();

        valid_                      = std::make_shared<bool>(true);
        std::shared_ptr<bool> valid = valid_;
        callback_                   = [this, valid](ResultType result) {
            if (!*valid)
                return;
            result_ = std::move(result);
            ready_  = true;
            on_event_ready();
        };

        async_op_(callback_);
    }

    ResultType
    await_resume() {
        unregister_suspended();
        awaiter_base::await_resume();
        return std::move(*result_);
    }

    ~async_awaiter() override {
        unschedule(); // full scrub: a fired-but-not-yet-resumed frame must leave ready_queue_/in_flight_ too
        if (valid_) {
            *valid_ = false;
        }
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_AWAITER_H
