/**
 * @file qb/io/async/coroutine/sync.h
 * @brief Synchronization primitives for coroutines
 *
 * This file provides asynchronous synchronization primitives:
 * - semaphore: Limit concurrent operations
 * - async_mutex: Mutual exclusion without blocking
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

#ifndef QB_IO_ASYNC_COROUTINE_SYNC_H
#define QB_IO_ASYNC_COROUTINE_SYNC_H

#include "cancellation.h" // cancellation_token / cancelled_error (cancellable acquire)
#include "task.h"
#include "utils.h"
// NOTE: No <mutex> needed — all primitives below are used exclusively within
// a single qb-io thread whose coroutines are cooperatively scheduled.
// Only one coroutine runs at a time; a suspension point (co_await) is the only
// place where another coroutine can run. This gives natural mutual exclusion
// without any OS-level lock. Adding std::mutex would only add overhead with
// zero benefit.
#include <cassert>
#include <deque>
#include <memory>

namespace qb::io::async {

// =============================================================================
// Semaphore
// =============================================================================

/**
 * @brief Asynchronous semaphore
 *
 * Semaphores limit concurrent access to resources.
 * Unlike std::semaphore, this version never blocks the thread.
 *
 * Usage:
 * @code
 * semaphore sem(5);  // Max 5 concurrent operations
 *
 * task<void> limited_op() {
 *     co_await sem.acquire();
 *     // Critical section - max 5 concurrent
 *     sem.release();
 * }
 * @endcode
 */
class semaphore {
public:
    /**
     * @brief Create semaphore with given number of permits
     * @param permits Number of available permits
     */
    explicit semaphore(size_t permits = 1)
        : _permits(permits)
        , _available(permits) {}

    /**
     * @brief A queued acquirer. Lives in its awaiter's frame; the deque holds a pointer to it so a
     *        cancelled waiter can be retracted (erased) and `release()` can mark it `granted`.
     */
    struct waiter_node {
        std::coroutine_handle<> h{};
        bool                    granted   = false; ///< set by release() when it hands this waiter a permit
        bool                    cancelled = false; ///< set by the cancel callback when it retracts the node
    };

    /**
     * @brief Awaiter for acquiring a permit
     *
     * No locking: single-thread cooperative scheduler guarantees only one
     * coroutine runs at a time. Between await_ready() and await_suspend()
     * no other coroutine can observe or modify _available / _waiters.
     */
    struct acquire_awaiter {
        semaphore  &sem;
        waiter_node node{}; // default-init so `acquire_awaiter{*this}` stays -Wmissing-field-initializers clean
        bool        _completed = false;

        // Finding 2.C.10: fast-path for the uncontended case. In a
        // mono-thread cooperative scheduler, `_available > 0` at await_ready()
        // time is a stable observation (no other coroutine can mutate state
        // between await_ready() and await_resume() without an intermediate
        // suspension). Taking the permit here lets the caller proceed
        // synchronously — no coroutine frame suspend/resume round-trip.
        [[nodiscard]] bool
        await_ready() noexcept {
            if (sem._available > 0) {
                --sem._available;
                ++sem._held;
                _completed = true;
                return true;
            }
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            node.h = h;
            // In the unlikely case where await_ready saw no permit but the
            // scheduler re-interleaved (it won't under the single-thread
            // model, but we keep the check for defensive robustness), grab
            // it here. Otherwise queue.
            if (sem._available > 0) {
                --sem._available;
                ++sem._held;
                _completed = true;
                schedule_via_current(h);
            } else {
                sem._waiters.push_back(&node);
            }
        }

        void
        await_resume() noexcept {}
    };

    /**
     * @brief Cancellation-aware acquirer: wakes (and throws `cancelled_error`) if `token` is
     *        cancelled while parked, **retracting** its queued claim so no permit is leaked.
     * @details Race-safe via `waiter_node::granted`: if `release()` hands this waiter a permit
     *          first, a later cancel is a no-op (the permit is honoured); if the cancel fires first,
     *          the node is erased from the queue so `release()` skips it and serves the next waiter.
     */
    struct cancel_acquire_awaiter {
        semaphore                  &sem;
        cancellation_token          token;
        waiter_node                 node;
        cancellation_token::id_type cancel_id = 0;
        std::shared_ptr<bool>       alive      = std::make_shared<bool>(true);
        bool                        _completed = false;

        cancel_acquire_awaiter(semaphore &s, cancellation_token t)
            : sem(s)
            , token(std::move(t)) {}

        ~cancel_acquire_awaiter() {
            if (alive)
                *alive = false; // neuter a callback still parked in the token after we are gone
            token.remove_on_cancel(cancel_id);
        }

        [[nodiscard]] bool
        await_ready() noexcept {
            if (token.is_cancelled())
                return true; // entry-cancelled → resume throws, no permit taken
            if (sem._available > 0) {
                --sem._available;
                ++sem._held;
                _completed = true;
                return true;
            }
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            node.h = h;
            if (token.is_cancelled()) {
                schedule_via_current(h);
                return;
            }
            if (sem._available > 0) {
                --sem._available;
                ++sem._held;
                _completed = true;
                schedule_via_current(h);
                return;
            }
            sem._waiters.push_back(&node);
            auto a    = alive;
            cancel_id = token.on_cancel([this, a]() {
                if (!*a || node.granted || node.cancelled)
                    return;                 // gone, already granted, or already retracted
                node.cancelled = true;
                sem.remove_waiter(&node);   // retract so release() will not hand us a permit
                schedule_via_current(node.h);
            });
        }

        void
        await_resume() {
            token.remove_on_cancel(cancel_id);
            cancel_id = 0;
            if (_completed || node.granted)
                return;                     // we hold a permit
            throw cancelled_error{};        // cancelled / entry-cancelled — never took a permit
        }
    };

    /**
     * @brief Acquire a permit (suspends if none available)
     */
    acquire_awaiter
    acquire() {
        return acquire_awaiter{*this};
    }

    /**
     * @brief Acquire a permit, cancellable via `token` (throws `cancelled_error` on cancel).
     * @param token A cancellation token (e.g. an actor scope, `ScopedCoroContext::token()`).
     * @details Unlike `acquire()`, a kill while parked unwinds cleanly and retracts the queued
     *          claim — no permit is leaked, and `release()` correctly serves the next waiter.
     */
    cancel_acquire_awaiter
    acquire(cancellation_token token) {
        return cancel_acquire_awaiter{*this, std::move(token)};
    }

    /**
     * @brief Try to acquire without suspending
     * @return true if permit was available and taken
     */
    bool
    try_acquire() {
        if (_available > 0) {
            --_available;
            ++_held;
            return true;
        }
        return false;
    }

    /**
     * @brief Release a permit
     *
     * If a waiter is queued, transfer the permit directly to it (no increment).
     * Over-release (calling release() without a matching acquire) is a no-op:
     * previously, a spurious release() while waiters were queued woke a waiter
     * with a phantom permit, breaking the permit invariant.
     */
    void
    release() {
        if (_held == 0)
            return; // over-release: nothing is actually held
        if (!_waiters.empty()) {
            auto *n = _waiters.front();
            _waiters.pop_front();
            // Direct hand-off: the permit transfers holder-to-holder, _held
            // stays constant (one releases, the woken waiter now holds it).
            // `granted` tells a cancel-aware waiter it owns the permit (so a
            // racing cancel becomes a no-op rather than leaking it).
            n->granted = true;
            schedule_via_current(n->h);
        } else {
            --_held;
            if (_available < _permits)
                ++_available;
        }
    }

    /**
     * @brief Number of currently available permits
     */
    size_t
    available_permits() const noexcept {
        return _available;
    }

    /**
     * @brief Total permits the semaphore was constructed with
     */
    size_t
    total_permits() const noexcept {
        return _permits;
    }

    /**
     * @brief RAII guard for semaphore
     */
    class guard {
        semaphore &_sem;
        bool       _released = false;

    public:
        explicit guard(semaphore &s)
            : _sem(s) {}

        // Non-copyable; move sets _released on the source so the moved-from
        // guard does NOT call release() again when it is destroyed.
        guard(const guard &)            = delete;
        guard &operator=(const guard &) = delete;

        guard(guard &&other) noexcept
            : _sem(other._sem)
            , _released(other._released) {
            other._released = true; // only the new owner releases
        }
        guard &operator=(guard &&) = delete;

        ~guard() {
            if (!_released)
                _sem.release();
        }

        void
        release() {
            if (!_released) {
                _sem.release();
                _released = true;
            }
        }
    };

    /**
     * @brief Scoped acquire - RAII style
     * @return Task that completes with guard
     */
    task<guard>
    scoped_acquire() {
        co_await acquire();
        co_return guard(*this);
    }

private:
    /// Retract a still-queued waiter (cancelled before being granted). O(n) over the (tiny) queue.
    void
    remove_waiter(waiter_node *n) noexcept {
        for (auto it = _waiters.begin(); it != _waiters.end(); ++it) {
            if (*it == n) {
                _waiters.erase(it);
                return;
            }
        }
    }

    size_t                    _permits;
    size_t                    _available;
    size_t                    _held = 0; /**< Permits currently held by acquirers (over-release guard). */
    std::deque<waiter_node *> _waiters;
};

// =============================================================================
// Async Mutex
// =============================================================================

/**
 * @brief Asynchronous mutex
 *
 * Mutual exclusion without blocking the thread.
 * Only one coroutine can hold the lock at a time.
 *
 * Usage:
 * @code
 * async_mutex mtx;
 *
 * task<void> protected_op() {
 *     co_await mtx.lock();
 *     // Critical section
 *     mtx.unlock();
 * }
 *
 * // Or with RAII:
 * task<void> protected_op() {
 *     auto lock = co_await mtx.scoped_lock();
 *     // Critical section - auto-unlocked on scope exit
 * }
 * @endcode
 */
class async_mutex {
public:
    /**
     * @brief Create unlocked mutex
     */
    async_mutex() = default;

    /**
     * @brief Awaiter for locking
     *
     * Lock-free (no OS mutex): single-thread cooperative scheduler ensures
     * only one coroutine is active at a time, giving natural mutual exclusion.
     */
    struct lock_awaiter {
        async_mutex &mtx;
        bool         _completed = false;

        [[nodiscard]] bool
        await_ready() const noexcept {
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            if (!mtx._locked) {
                mtx._locked = true;
                _completed  = true;
                schedule_via_current(h);
            } else {
                mtx._waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {}
    };

    /**
     * @brief Acquire lock (suspends if already held)
     */
    lock_awaiter
    lock() {
        return lock_awaiter{*this};
    }

    /**
     * @brief Try to lock without suspending
     */
    bool
    try_lock() {
        if (!_locked) {
            _locked = true;
            return true;
        }
        return false;
    }

    /**
     * @brief Release lock; wakes next waiter if any
     *
     * Finding 2.C.11: unlocking an unlocked mutex is a programming error;
     * debug builds now assert on it to catch missing `scoped_lock()` usage.
     */
    void
    unlock() {
        assert(_locked && "async_mutex::unlock called on an unlocked mutex");
        if (_waiters.empty()) {
            _locked = false;
        } else {
            auto h = _waiters.front();
            _waiters.pop_front();
            schedule_via_current(h);
        }
    }

    /**
     * @brief Check if the mutex is currently held
     */
    bool
    is_locked() const noexcept {
        return _locked;
    }

    /**
     * @brief RAII lock guard
     */
    class guard {
        async_mutex &_mtx;
        bool         _released = false;

    public:
        explicit guard(async_mutex &m)
            : _mtx(m) {}

        guard(const guard &)            = delete;
        guard &operator=(const guard &) = delete;

        guard(guard &&other) noexcept
            : _mtx(other._mtx)
            , _released(other._released) {
            other._released = true;
        }

        ~guard() {
            if (!_released) {
                _mtx.unlock();
            }
        }

        void
        unlock() {
            if (!_released) {
                _mtx.unlock();
                _released = true;
            }
        }
    };

    /**
     * @brief Scoped lock acquisition - RAII style
     * @return Task that completes with guard
     *
     * Usage:
     * @code
     * task<void> op() {
     *     auto lock = co_await mtx.scoped_lock();
     *     // Critical section
     * }  // Auto-unlocked here
     * @endcode
     */
    task<guard>
    scoped_lock() {
        co_await lock();
        co_return guard(*this);
    }

    /**
     * @brief Number of coroutines waiting for the lock
     */
    size_t
    waiters_count() const noexcept {
        return _waiters.size();
    }

private:
    bool                                _locked = false;
    std::deque<std::coroutine_handle<>> _waiters;
};

// =============================================================================
// Read-Write Lock
// =============================================================================

/**
 * @brief Asynchronous read-write lock
 *
 * Multiple readers or single writer allowed.
 *
 * Usage:
 * @code
 * async_rw_lock rw;
 *
 * // Reader
 * task<void> read_op() {
 *     co_await rw.lock_read();
 *     // Read-only access
 *     rw.unlock_read();
 * }
 *
 * // Writer
 * task<void> write_op() {
 *     co_await rw.lock_write();
 *     // Exclusive write access
 *     rw.unlock_write();
 * }
 * @endcode
 */
class async_rw_lock {
public:
    /**
     * @brief Awaiter for read lock
     */
    struct read_lock_awaiter {
        async_rw_lock &rw;
        bool           _completed = false;

        [[nodiscard]] bool
        await_ready() const noexcept {
            return false;
        }

        // No OS lock: single-thread cooperative scheduler.
        void
        await_suspend(std::coroutine_handle<> h) {
            if (!rw._write_locked && rw._write_waiters.empty()) {
                ++rw._readers;
                _completed = true;
                schedule_via_current(h);
            } else {
                rw._read_waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {}
    };

    struct write_lock_awaiter {
        async_rw_lock &rw;
        bool           _completed = false;

        [[nodiscard]] bool
        await_ready() const noexcept {
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            if (!rw._write_locked && rw._readers == 0) {
                rw._write_locked = true;
                _completed       = true;
                schedule_via_current(h);
            } else {
                rw._write_waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {}
    };

    read_lock_awaiter
    lock_read() {
        return read_lock_awaiter{*this};
    }

    write_lock_awaiter
    lock_write() {
        return write_lock_awaiter{*this};
    }

    void
    unlock_read() {
        assert(_readers > 0 && "async_rw_lock::unlock_read called with no read lock held");
        if (_readers == 0)
            return; // release build: ignore instead of size_t underflow
        --_readers;
        if (_readers == 0 && !_write_waiters.empty()) {
            auto h = _write_waiters.front();
            _write_waiters.pop_front();
            _write_locked = true;
            schedule_via_current(h);
        }
    }

    void
    unlock_write() {
        assert(_write_locked && "async_rw_lock::unlock_write called with no write lock held");
        _write_locked = false;
        // Prefer pending readers; fall back to the next writer.
        if (!_read_waiters.empty()) {
            for (auto h : _read_waiters) {
                ++_readers;
                schedule_via_current(h);
            }
            _read_waiters.clear();
        } else if (!_write_waiters.empty()) {
            auto h = _write_waiters.front();
            _write_waiters.pop_front();
            _write_locked = true;
            schedule_via_current(h);
        }
    }

    /**
     * @brief RAII read guard
     *
     * Move constructor must set other._released = true so the moved-from
     * guard does NOT call unlock_read() again on destruction.  Without this,
     * task<read_guard> moves the guard out of the promise on co_await, but
     * the moved-from guard in the promise still has _released = false and
     * would fire a phantom unlock_read() when the promise is destroyed.
     */
    class read_guard {
        async_rw_lock &_rw;
        bool           _released = false;

    public:
        explicit read_guard(async_rw_lock &rw)
            : _rw(rw) {}

        read_guard(const read_guard &)            = delete;
        read_guard &operator=(const read_guard &) = delete;

        read_guard(read_guard &&other) noexcept
            : _rw(other._rw)
            , _released(other._released) {
            other._released = true; // only the new owner unlocks
        }
        read_guard &operator=(read_guard &&) = delete;

        ~read_guard() {
            if (!_released)
                _rw.unlock_read();
        }
        void
        unlock() {
            if (!_released) {
                _rw.unlock_read();
                _released = true;
            }
        }
    };

    /**
     * @brief RAII write guard
     *
     * Same move-constructor fix as read_guard — prevents phantom unlock_write()
     * from the moved-from guard stored in the task<write_guard> promise.
     */
    class write_guard {
        async_rw_lock &_rw;
        bool           _released = false;

    public:
        explicit write_guard(async_rw_lock &rw)
            : _rw(rw) {}

        write_guard(const write_guard &)            = delete;
        write_guard &operator=(const write_guard &) = delete;

        write_guard(write_guard &&other) noexcept
            : _rw(other._rw)
            , _released(other._released) {
            other._released = true; // only the new owner unlocks
        }
        write_guard &operator=(write_guard &&) = delete;

        ~write_guard() {
            if (!_released)
                _rw.unlock_write();
        }
        void
        unlock() {
            if (!_released) {
                _rw.unlock_write();
                _released = true;
            }
        }
    };

    task<read_guard>
    scoped_read_lock() {
        co_await lock_read();
        co_return read_guard(*this);
    }

    task<write_guard>
    scoped_write_lock() {
        co_await lock_write();
        co_return write_guard(*this);
    }

private:
    bool                                _write_locked = false;
    size_t                              _readers      = 0;
    std::deque<std::coroutine_handle<>> _read_waiters;
    std::deque<std::coroutine_handle<>> _write_waiters;
};

// =============================================================================
// Barrier
// =============================================================================

/**
 * @brief Asynchronous barrier
 *
 * Waits for N coroutines to reach the barrier before releasing all.
 *
 * Usage:
 * @code
 * barrier b(3);  // Wait for 3 coroutines
 *
 * task<void> worker() {
 *     // Do work phase 1
 *     co_await b.arrive_and_wait();
 *     // Do work phase 2 (all workers here)
 * }
 * @endcode
 */
class barrier {
public:
    explicit barrier(size_t count)
        : _expected(count)
        , _remaining(count) {}

    struct arrive_awaiter {
        barrier &b;

        // Single-thread cooperative: _remaining won't change between
        // await_ready() and await_suspend() since we haven't suspended yet.
        [[nodiscard]] bool
        await_ready() const noexcept {
            return b._remaining == 0;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            // Finding 2.C.12: help users spot barriers that were not reset
            // between phases — an unexpected await after all arrivals have
            // happened is almost always a missing `reset()`.
            if (b._remaining == 0) {
                schedule_via_current(h);
                return;
            }

            b._waiters.push_back(h);

            if (--b._remaining == 0) {
                for (auto w : b._waiters)
                    schedule_via_current(w);
                b._waiters.clear();
            }
        }

        void
        await_resume() noexcept {}
    };

    arrive_awaiter
    arrive_and_wait() {
        return arrive_awaiter{*this};
    }

    /**
     * @brief Reset barrier for reuse
     */
    void
    reset() {
        _remaining = _expected;
        _waiters.clear();
    }

private:
    size_t                               _expected;
    size_t                               _remaining;
    std::vector<std::coroutine_handle<>> _waiters;
};

// =============================================================================
// Async Event
// =============================================================================

/**
 * @brief Asynchronous event — signals waiting coroutines without blocking
 *
 * Two modes:
 *  - Manual-reset (default): set() wakes ALL waiters and stays set.
 *    New waiters after set() return immediately until reset() is called.
 *  - Auto-reset: set() wakes exactly ONE waiter (or records the signal for
 *    the next waiter), behaving like a binary semaphore.
 *
 * Usage:
 * @code
 * async_event ready;
 *
 * task<void> producer() {
 *     co_await do_work();
 *     ready.set();          // broadcast to all waiters
 * }
 *
 * task<void> consumer() {
 *     co_await ready.wait();  // suspends until set()
 *     use_result();
 * }
 * @endcode
 *
 * With cancellation:
 * @code
 * async_event ev;
 * task<void> consumer(cancellation_token tok) {
 *     co_await when_any(ev.wait(), check_cancelled(tok));
 * }
 * @endcode
 */
class async_event {
public:
    /**
     * @param auto_reset     if true, each set() wakes exactly one waiter
     * @param initially_set  if true, first wait() returns immediately
     */
    explicit async_event(bool auto_reset = false, bool initially_set = false) noexcept
        : _signaled(initially_set)
        , _auto_reset(auto_reset) {}

    // Non-copyable (waiters hold a pointer to this)
    async_event(const async_event &)            = delete;
    async_event &operator=(const async_event &) = delete;

    /**
     * @brief Awaiter — suspends until the event is set
     *
     * Cooperative single-thread: no lock needed; only one coroutine
     * executes between await_ready() and await_suspend().
     */
    struct wait_awaiter {
        async_event &_ev;

        [[nodiscard]] bool
        await_ready() noexcept {
            if (_ev._signaled) {
                if (_ev._auto_reset)
                    _ev._signaled = false;
                return true;
            }
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            if (_ev._signaled) {
                if (_ev._auto_reset)
                    _ev._signaled = false;
                schedule_via_current(h);
            } else {
                _ev._waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {}
    };

    /** @brief Suspend until the event is set */
    wait_awaiter
    wait() {
        return wait_awaiter{*this};
    }

    /**
     * @brief Signal the event
     *
     * Manual-reset: wakes all current waiters and marks event as set.
     * Auto-reset:   wakes one waiter, or records the signal if no waiter.
     */
    void
    set() {
        if (_auto_reset) {
            if (!_waiters.empty()) {
                auto h = _waiters.front();
                _waiters.pop_front();
                schedule_via_current(h);
            } else {
                _signaled = true;
            }
        } else {
            _signaled    = true;
            auto waiters = std::move(_waiters);
            for (auto h : waiters)
                schedule_via_current(h);
        }
    }

    /** @brief Clear the event (only meaningful in manual-reset mode) */
    void
    reset() noexcept {
        _signaled = false;
    }

    bool
    is_set() const noexcept {
        return _signaled;
    }
    size_t
    waiters_count() const noexcept {
        return _waiters.size();
    }

private:
    bool                                _signaled;
    bool                                _auto_reset;
    std::deque<std::coroutine_handle<>> _waiters;
};

// =============================================================================
// Async Latch (one-shot countdown)
// =============================================================================

/**
 * @brief One-shot countdown synchronization point
 *
 * A latch starts with a count > 0 and decrements toward zero. Once it reaches
 * zero all waiters are released and the latch stays open (cannot be reset —
 * use barrier for reusable rendezvous).
 *
 * Usage:
 * @code
 * async_latch latch(3);
 *
 * task<void> worker(async_latch& l) {
 *     co_await do_work();
 *     l.count_down();          // decrement; releases all when reaching 0
 * }
 *
 * task<void> orchestrator() {
 *     scope.spawn(worker(latch));
 *     scope.spawn(worker(latch));
 *     scope.spawn(worker(latch));
 *     co_await latch.wait();   // waits until all three decremented
 * }
 * @endcode
 */
class async_latch {
public:
    explicit async_latch(size_t count) noexcept
        : _count(count) {}

    // Non-copyable (waiters hold a pointer)
    async_latch(const async_latch &)            = delete;
    async_latch &operator=(const async_latch &) = delete;

    /**
     * @brief Decrement the counter by n (default 1)
     *
     * Releases all waiters if the counter reaches zero.
     * Safe to call extra times after reaching zero (no-op).
     */
    void
    count_down(size_t n = 1) noexcept {
        if (_count == 0)
            return;
        _count = (n >= _count) ? 0 : _count - n;
        if (_count == 0) {
            auto w = std::move(_waiters);
            for (auto h : w)
                schedule_via_current(h);
        }
    }

    bool
    is_ready() const noexcept {
        return _count == 0;
    }
    size_t
    current_count() const noexcept {
        return _count;
    }

    struct wait_awaiter {
        async_latch &_latch;
        [[nodiscard]] bool
        await_ready() const noexcept {
            return _latch._count == 0;
        }
        void
        await_suspend(std::coroutine_handle<> h) {
            if (_latch._count == 0)
                schedule_via_current(h);
            else
                _latch._waiters.push_back(h);
        }
        void
        await_resume() noexcept {}
    };

    /** @brief Suspend until the count reaches zero */
    wait_awaiter
    wait() {
        return wait_awaiter{*this};
    }

    /** @brief count_down(1) then wait() combined */
    task<void>
    arrive_and_wait() {
        count_down();
        co_await wait();
    }

private:
    size_t                               _count;
    std::vector<std::coroutine_handle<>> _waiters;
};

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Execute function with semaphore limit
 * @tparam F Function type
 * @param sem Semaphore to use
 * @param f Function to execute
 * @return Task with function result
 * @ingroup Coroutine
 */
template <typename F>
auto
with_semaphore(semaphore &sem, F f) -> task<std::invoke_result_t<F>> {
    co_await sem.acquire();

    // Use RAII guard pattern manually
    struct guard {
        semaphore &s;
        ~guard() {
            s.release();
        }
    } g{sem};

    co_return f();
}

/**
 * @brief Execute function with mutex lock
 * @tparam F Function type
 * @param mtx Mutex to use
 * @param f Function to execute
 * @return Task with function result
 * @ingroup Coroutine
 */
template <typename F>
auto
with_lock(async_mutex &mtx, F f) -> task<std::invoke_result_t<F>> {
    auto guard = co_await mtx.scoped_lock();
    co_return f();
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SYNC_H
