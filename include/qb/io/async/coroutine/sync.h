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

    // Mark the semaphore dead so a parked awaiter destroyed *after* the semaphore (e.g. both
    // owned by a scope torn down in this order) skips the now-freed `_waiters`. Allocated lazily
    // on the first park, so the uncontended path stays allocation-free (see `_alive`).
    ~semaphore() {
        if (_alive)
            *_alive = false;
    }
    semaphore(const semaphore &)            = delete;
    semaphore &operator=(const semaphore &) = delete;

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
        semaphore            &sem;
        waiter_node           node{}; // default-init so the brace-init stays -Wmissing-field-initializers clean
        bool                  _completed = false;
        bool                  _parked    = false;
        bool                  _resumed   = false; ///< set in await_resume: distinguishes granted-then-reclaimed
        std::shared_ptr<bool> _sem_alive; ///< semaphore liveness; skip retract when false

        // User-declared dtor below makes this a non-aggregate → provide the ctor `acquire()` uses.
        explicit acquire_awaiter(semaphore &s)
            : sem(s) {}

        // Destroyed while still queued, not yet granted (e.g. a when_any/race loser reclaim):
        // retract our node so a later release() cannot write `granted`/schedule a freed handle.
        // If we were already GRANTED (release() popped our node and handed us the permit) but never
        // resumed — the granted-then-reclaimed window of a when_any/with_deadline loser — the permit
        // we were handed would otherwise be lost forever (the semaphore's effective capacity erodes
        // by one). Hand it back via release() so the next waiter is served.
        ~acquire_awaiter() {
            if (!_parked || _resumed || !_sem_alive || !*_sem_alive)
                return;
            if (node.granted)
                sem.release();          // return the permit we were granted but never consumed
            else
                sem.remove_waiter(&node);
        }

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
                _sem_alive = sem.park_alive();
                _parked    = true;
                sem._waiters.push_back(&node);
            }
        }

        void
        await_resume() noexcept {
            _resumed = true;
        }
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
        cancellation_token::id_type cancel_id  = 0;
        std::shared_ptr<bool>       alive      = std::make_shared<bool>(true);
        std::shared_ptr<bool>       _sem_alive; ///< semaphore liveness; skip retract when false
        bool                        _completed = false;
        bool                        _parked    = false;
        bool                        _resumed   = false; ///< set in await_resume: distinguishes granted-then-reclaimed

        cancel_acquire_awaiter(semaphore &s, cancellation_token t)
            : sem(s)
            , token(std::move(t)) {}

        ~cancel_acquire_awaiter() {
            if (alive)
                *alive = false; // neuter a callback still parked in the token after we are gone
            token.remove_on_cancel(cancel_id);
            if (!_parked || _resumed || !_sem_alive || !*_sem_alive)
                return;
            // Destroyed while still queued/granted but never resumed (an OUTER when_any/race loser
            // reclaim — distinct from this token's own cancel, which sets node.cancelled and resumes).
            if (node.granted)
                sem.release();            // return the granted-but-unconsumed permit to the next waiter
            else if (!node.cancelled)
                sem.remove_waiter(&node); // still parked: retract so release() cannot touch a freed frame
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
            _sem_alive = sem.park_alive();
            _parked    = true;
            sem._waiters.push_back(&node);
            auto a    = alive;
            cancel_id = token.on_cancel([this, a]() {
                if (!*a || node.granted || node.cancelled)
                    return; // gone, already granted, or already retracted
                node.cancelled = true;
                sem.remove_waiter(&node); // retract so release() will not hand us a permit
                schedule_via_current(node.h);
            });
        }

        void
        await_resume() {
            _resumed = true;
            token.remove_on_cancel(cancel_id);
            cancel_id = 0;
            if (_completed || node.granted)
                return;              // we hold a permit
            throw cancelled_error{}; // cancelled / entry-cancelled — never took a permit
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

    /// Liveness token shared with parked awaiters, lazily created on the first park so the
    /// uncontended fast path allocates nothing. An awaiter destroyed after this semaphore checks
    /// `*_alive == false` and skips the freed `_waiters`.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    size_t                    _permits;
    size_t                    _available;
    size_t                    _held = 0; /**< Permits currently held by acquirers (over-release guard). */
    std::deque<waiter_node *> _waiters;
    std::shared_ptr<bool>     _alive; ///< lazily allocated on first park; set false in dtor
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

    // Waiters hold a handle queued in `_waiters` tied to this object → non-copyable/non-movable.
    // The dtor marks the lazily-allocated liveness token dead so an awaiter destroyed *after* the
    // mutex skips the freed `_waiters`.
    ~async_mutex() {
        if (_alive)
            *_alive = false;
    }
    async_mutex(const async_mutex &)            = delete;
    async_mutex &operator=(const async_mutex &) = delete;

    /**
     * @brief Awaiter for locking
     *
     * Lock-free (no OS mutex): single-thread cooperative scheduler ensures
     * only one coroutine is active at a time, giving natural mutual exclusion.
     */
    struct lock_awaiter {
        async_mutex            &mtx;
        bool                    _completed = false;
        bool                    _resumed   = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::coroutine_handle<> _parked{};   ///< set when queued in _waiters
        std::shared_ptr<bool>   _mtx_alive;   ///< mutex liveness; skip retract when false

        // User-declared dtor below makes this a non-aggregate → provide the ctor `lock()` uses.
        explicit lock_awaiter(async_mutex &m)
            : mtx(m) {}

        // Destroyed while still parked (e.g. a when_any/race loser reclaim): retract our handle so
        // a later unlock() cannot schedule a freed frame. If we were already WOKEN (unlock() popped
        // us and handed the lock to us) but never resumed — the woken-then-reclaimed window of a
        // when_any/with_deadline loser — the lock ownership we were handed would otherwise be lost
        // forever (mutex stuck `_locked` with no holder). Release it so the next waiter is served.
        ~lock_awaiter() {
            if (!_parked || _resumed || !_mtx_alive || !*_mtx_alive)
                return;
            if (!mtx.remove_parked(_parked)) // not in _waiters ⇒ unlock() already handed us the lock
                mtx.unlock();                // give the abandoned ownership back to the next waiter
        }

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
                _mtx_alive = mtx.park_alive();
                _parked    = h;
                mtx._waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {
            _resumed = true;
        }
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
    /// Retract a still-queued waiter handle (destroyed before being granted). O(n), tiny queue.
    /// @return true if the handle was found and erased (it was still parked); false if it was not in
    ///         the queue (already woken/handed the lock by unlock(), or already resumed).
    bool
    remove_parked(std::coroutine_handle<> h) noexcept {
        for (auto it = _waiters.begin(); it != _waiters.end(); ++it) {
            if (*it == h) {
                _waiters.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Liveness token shared with parked awaiters, lazily created on the first park so the
    /// uncontended lock/unlock path allocates nothing.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    bool                                _locked = false;
    std::deque<std::coroutine_handle<>> _waiters;
    std::shared_ptr<bool>               _alive; ///< lazily allocated on first park; set false in dtor
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
    async_rw_lock() = default;

    // Waiters hold handles queued in the wait lists tied to this object → non-copyable/non-movable.
    ~async_rw_lock() {
        if (_alive)
            *_alive = false;
    }
    async_rw_lock(const async_rw_lock &)            = delete;
    async_rw_lock &operator=(const async_rw_lock &) = delete;

    /**
     * @brief Awaiter for read lock
     */
    struct read_lock_awaiter {
        async_rw_lock          &rw;
        bool                    _completed = false;
        bool                    _resumed   = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::coroutine_handle<> _parked{};  ///< set when queued in _read_waiters
        std::shared_ptr<bool>   _rw_alive;   ///< lock liveness; skip retract when false

        explicit read_lock_awaiter(async_rw_lock &r)
            : rw(r) {}

        // Destroyed while still parked (when_any/race loser reclaim): retract our handle so a later
        // unlock_write() cannot schedule a freed frame. If we were already WOKEN (unlock_write()
        // admitted us as a reader: ++_readers, popped us) but never resumed — the woken-then-reclaimed
        // window of a when_any/with_deadline loser — that reader count would otherwise leak forever
        // (a future writer waits on _readers==0 that never comes). Give the read slot back.
        ~read_lock_awaiter() {
            if (!_parked || _resumed || !_rw_alive || !*_rw_alive)
                return;
            if (!erase_handle(rw._read_waiters, _parked)) // not parked ⇒ unlock_write() admitted us
                rw.unlock_read();                         // return the abandoned reader slot
        }

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
                _rw_alive = rw.park_alive();
                _parked   = h;
                rw._read_waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {
            _resumed = true;
        }
    };

    struct write_lock_awaiter {
        async_rw_lock          &rw;
        bool                    _completed = false;
        bool                    _resumed   = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::coroutine_handle<> _parked{};  ///< set when queued in _write_waiters
        std::shared_ptr<bool>   _rw_alive;   ///< lock liveness; skip retract when false

        explicit write_lock_awaiter(async_rw_lock &r)
            : rw(r) {}

        // Destroyed while still parked: retract. If we were already WOKEN (unlock_read()/unlock_write()
        // handed us the write lock: _write_locked=true, popped us) but never resumed — the
        // woken-then-reclaimed window of a when_any/with_deadline loser — the lock would otherwise be
        // stuck `_write_locked` with no holder forever. Release it so the next waiter is served.
        ~write_lock_awaiter() {
            if (!_parked || _resumed || !_rw_alive || !*_rw_alive)
                return;
            if (!erase_handle(rw._write_waiters, _parked)) // not parked ⇒ we were handed the write lock
                rw.unlock_write();                         // give the abandoned write lock back
        }

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
                _rw_alive = rw.park_alive();
                _parked   = h;
                rw._write_waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {
            _resumed = true;
        }
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
    /// Retract a still-queued waiter handle from one of the wait lists. O(n), tiny queue.
    /// @return true if found+erased (still parked); false if absent (already woken/admitted, or resumed).
    static bool
    erase_handle(std::deque<std::coroutine_handle<>> &q, std::coroutine_handle<> h) noexcept {
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (*it == h) {
                q.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Liveness token shared with parked awaiters, lazily created on the first park so the
    /// uncontended lock/unlock path allocates nothing.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    bool                                _write_locked = false;
    size_t                              _readers      = 0;
    std::deque<std::coroutine_handle<>> _read_waiters;
    std::deque<std::coroutine_handle<>> _write_waiters;
    std::shared_ptr<bool>               _alive; ///< lazily allocated on first park; set false in dtor
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

    // Waiters hold handles queued in `_waiters` tied to this object → non-copyable/non-movable.
    ~barrier() {
        if (_alive)
            *_alive = false;
    }
    barrier(const barrier &)            = delete;
    barrier &operator=(const barrier &) = delete;

    struct arrive_awaiter {
        barrier                &b;
        std::coroutine_handle<> _parked{};  ///< set when queued in _waiters
        std::shared_ptr<bool>   _b_alive;    ///< barrier liveness; skip retract when false

        explicit arrive_awaiter(barrier &bar)
            : b(bar) {}

        // Destroyed while still parked (a non-final arrival reclaimed by when_any/race): retract our
        // handle so the final arrival cannot schedule a freed frame. (The arrival already counted —
        // _remaining stays decremented, matching "an arrived-then-gone waiter still arrived".)
        ~arrive_awaiter() {
            if (_parked && _b_alive && *_b_alive)
                b.erase_handle(_parked);
        }

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

            _b_alive = b.park_alive();
            _parked  = h;
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
    /// Retract a still-queued waiter handle (a reclaimed non-final arrival). O(n), tiny queue.
    void
    erase_handle(std::coroutine_handle<> h) noexcept {
        for (auto it = _waiters.begin(); it != _waiters.end(); ++it) {
            if (*it == h) {
                _waiters.erase(it);
                return;
            }
        }
    }

    /// Liveness token shared with parked awaiters, lazily created on the first park.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    size_t                               _expected;
    size_t                               _remaining;
    std::vector<std::coroutine_handle<>> _waiters;
    std::shared_ptr<bool>                _alive; ///< lazily allocated on first park; set false in dtor
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

    ~async_event() {
        if (_alive)
            *_alive = false;
    }

    /**
     * @brief Awaiter — suspends until the event is set
     *
     * Cooperative single-thread: no lock needed; only one coroutine
     * executes between await_ready() and await_suspend().
     */
    struct wait_awaiter {
        async_event            &_ev;
        bool                    _resumed = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::coroutine_handle<> _parked{};  ///< set when queued in _waiters
        std::shared_ptr<bool>   _ev_alive;   ///< event liveness; skip retract when false

        explicit wait_awaiter(async_event &ev)
            : _ev(ev) {}

        // Destroyed while still parked (when_any/race loser reclaim): retract our handle so a later
        // set() cannot schedule a freed frame. For an AUTO-RESET event, if we were already WOKEN
        // (set() consumed the one-shot signal to wake us and popped us) but never resumed — the
        // woken-then-reclaimed window of a when_any/with_deadline loser — that signal would otherwise
        // be lost forever (the next waiter parks indefinitely). Re-deliver it via set(). Manual-reset
        // keeps _signaled latched, so nothing is consumed and no re-signal is needed there.
        ~wait_awaiter() {
            if (!_parked || _resumed || !_ev_alive || !*_ev_alive)
                return;
            if (!_ev.erase_handle(_parked) && _ev._auto_reset) // not parked ⇒ set() consumed a signal for us
                _ev.set();                                     // re-deliver the abandoned wake
        }

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
                _ev_alive = _ev.park_alive();
                _parked   = h;
                _ev._waiters.push_back(h);
            }
        }

        void
        await_resume() noexcept {
            _resumed = true;
        }
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
    /// Retract a still-queued waiter handle (a reclaimed waiter). O(n), tiny queue.
    /// @return true if found+erased (still parked); false if absent (already woken by set(), or resumed).
    bool
    erase_handle(std::coroutine_handle<> h) noexcept {
        for (auto it = _waiters.begin(); it != _waiters.end(); ++it) {
            if (*it == h) {
                _waiters.erase(it);
                return true;
            }
        }
        return false;
    }

    /// Liveness token shared with parked awaiters, lazily created on the first park.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    bool                                _signaled;
    bool                                _auto_reset;
    std::deque<std::coroutine_handle<>> _waiters;
    std::shared_ptr<bool>               _alive; ///< lazily allocated on first park; set false in dtor
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

    ~async_latch() {
        if (_alive)
            *_alive = false;
    }

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
        async_latch            &_latch;
        std::coroutine_handle<> _parked{};   ///< set when queued in _waiters
        std::shared_ptr<bool>   _latch_alive; ///< latch liveness; skip retract when false

        explicit wait_awaiter(async_latch &l)
            : _latch(l) {}

        // Destroyed while still parked (when_any/race loser reclaim): retract our handle so a later
        // count_down()-to-zero cannot schedule a freed frame.
        ~wait_awaiter() {
            if (_parked && _latch_alive && *_latch_alive)
                _latch.erase_handle(_parked);
        }

        [[nodiscard]] bool
        await_ready() const noexcept {
            return _latch._count == 0;
        }
        void
        await_suspend(std::coroutine_handle<> h) {
            if (_latch._count == 0)
                schedule_via_current(h);
            else {
                _latch_alive = _latch.park_alive();
                _parked      = h;
                _latch._waiters.push_back(h);
            }
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
    /// Retract a still-queued waiter handle (a reclaimed waiter). O(n), tiny queue.
    void
    erase_handle(std::coroutine_handle<> h) noexcept {
        for (auto it = _waiters.begin(); it != _waiters.end(); ++it) {
            if (*it == h) {
                _waiters.erase(it);
                return;
            }
        }
    }

    /// Liveness token shared with parked awaiters, lazily created on the first park.
    std::shared_ptr<bool> &
    park_alive() {
        if (!_alive)
            _alive = std::make_shared<bool>(true);
        return _alive;
    }

    size_t                               _count;
    std::vector<std::coroutine_handle<>> _waiters;
    std::shared_ptr<bool>                _alive; ///< lazily allocated on first park; set false in dtor
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
