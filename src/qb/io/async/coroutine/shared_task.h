/**
 * @file qb/io/async/coroutine/shared_task.h
 * @brief shared_task<T> — multi-consumer coroutine result
 *
 * A shared_task<T> wraps a task<T> whose result can be awaited by any number
 * of coroutines. The first co_await triggers execution; all subsequent ones
 * wait for the same result (no recomputation).
 *
 * Key properties:
 *  - Copyable handle: holds a shared_ptr to the internal state
 *  - Single execution: the underlying task runs exactly once
 *  - Any number of awaiters: all receive the same value (or exception)
 *  - Zero mutex: single-thread cooperative model
 *
 * Usage:
 * @code
 * shared_task<int> shared = make_shared_task(compute_value());
 *
 * task<void> reader_a() { int v = co_await shared; }
 * task<void> reader_b() { int v = co_await shared; }
 * @endcode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0
 */

#pragma once
#ifndef QB_IO_ASYNC_COROUTINE_SHARED_TASK_H
#define QB_IO_ASYNC_COROUTINE_SHARED_TASK_H

#include <cassert>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include "task.h"
#include "scheduler.h"

namespace qb::io::async {

// Forward declaration
template <typename T>
class shared_task;
template <typename T>
shared_task<T> make_shared_task(task<T> &&t);

/**
 * @brief shared_task<T> — copyable, multi-awaiter coroutine handle
 * @tparam T Result type (non-void)
 */
template <typename T>
class shared_task {
    // -----------------------------------------------------------------------
    // Shared state (heap-allocated, reference-counted)
    // -----------------------------------------------------------------------
    struct state {
        enum class status { pending, ready, failed };

        status                               _status{status::pending};
        std::optional<T>                     _value;
        std::exception_ptr                   _error;
        std::vector<std::coroutine_handle<>> _waiters;

        state() {
            // Finding 2.A.6: pre-reserve a small capacity so that the
            // common "1–4 concurrent awaiters" case never reallocates in
            // `await_suspend`, which must be noexcept (throwing after
            // suspension is implementation-defined and unsafe).
            _waiters.reserve(4);
        }

        void
        complete(T val) {
            _value.emplace(std::move(val));
            _status = status::ready;
            flush();
        }

        void
        fail(std::exception_ptr e) {
            _error  = e;
            _status = status::failed;
            flush();
        }

        bool
        is_done() const noexcept {
            return _status != status::pending;
        }

        T
        get() const {
            if (_status == status::failed)
                std::rethrow_exception(_error);
            return *_value;
        }

    private:
        void
        flush() {
            auto w = std::move(_waiters);
            for (auto h : w)
                schedule_via_current(h);
        }
    };

    std::shared_ptr<state> _state;

    explicit shared_task(std::shared_ptr<state> s)
        : _state(std::move(s)) {}

    // The runner coroutine: runs the inner task, then notifies all waiters.
    // Parameters are stored by value in the coroutine frame (safe).
    static task<void>
    runner(task<T> inner, std::shared_ptr<state> s) {
        try {
            s->complete(co_await std::move(inner));
        } catch (...) {
            s->fail(std::current_exception());
        }
    }

public:
    // Default-constructed handle is invalid (null state)
    shared_task() = default;

    // Copyable: all copies share the same computation
    shared_task(const shared_task &)            = default;
    shared_task &operator=(const shared_task &) = default;
    shared_task(shared_task &&)                 = default;
    shared_task &operator=(shared_task &&)      = default;

    [[nodiscard]] bool
    valid() const noexcept {
        return _state != nullptr;
    }
    [[nodiscard]] bool
    is_ready() const noexcept {
        return _state && _state->is_done();
    }

    // -----------------------------------------------------------------------
    // Awaiter
    // -----------------------------------------------------------------------
    struct awaiter {
        std::shared_ptr<state>  s;
        std::coroutine_handle<> _parked{}; ///< set when queued in s->_waiters

        explicit awaiter(std::shared_ptr<state> st)
            : s(std::move(st)) {}

        // Destroyed while still parked (e.g. a when_any/race loser reclaim): retract our handle so a
        // later flush() cannot schedule a freed frame. The held `s` keeps the state alive, so no
        // separate liveness guard is needed — the list we erase from is always valid here.
        ~awaiter() {
            if (!_parked || !s)
                return;
            auto &w = s->_waiters;
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (*it == _parked) {
                    w.erase(it);
                    break;
                }
            }
        }

        [[nodiscard]] bool
        await_ready() const noexcept {
            // Finding 2.A.1: reject `co_await` on a default-constructed /
            // moved-from shared_task loudly. Returning `true` here would
            // make `await_resume` dereference a null state (UB). We return
            // false so the suspend path can throw instead.
            return s ? s->is_done() : false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            // Same safety check — if the state is null, the caller
            // `co_await`ed an invalid handle. Fail loudly rather than
            // silently corrupt.
            if (!s) {
                throw std::logic_error("co_await on default-constructed shared_task<T>");
            }
            if (s->is_done()) {
                schedule_via_current(h);
                return;
            }
            // Pre-reserved capacity (see state::state) makes push_back
            // effectively noexcept for the common case.
            _parked = h;
            s->_waiters.push_back(h);
        }

        T
        await_resume() {
            if (!s) {
                throw std::logic_error("co_await on default-constructed shared_task<T>");
            }
            return s->get();
        }
    };

    awaiter
    operator co_await() const {
        return awaiter{_state};
    }

    friend shared_task<T> make_shared_task<T>(task<T> &&t);
};

// =============================================================================
// shared_task<void> specialisation
// =============================================================================

template <>
class shared_task<void> {
    struct state {
        enum class status { pending, ready, failed };
        status                               _status{status::pending};
        std::exception_ptr                   _error;
        std::vector<std::coroutine_handle<>> _waiters;

        state() {
            // Finding 2.A.6: pre-reserve to keep await_suspend noexcept for
            // the common case (see the non-void specialisation for details).
            _waiters.reserve(4);
        }

        void
        complete() {
            _status = status::ready;
            flush();
        }
        void
        fail(std::exception_ptr e) {
            _error  = e;
            _status = status::failed;
            flush();
        }
        bool
        is_done() const noexcept {
            return _status != status::pending;
        }
        void
        get() const {
            if (_status == status::failed)
                std::rethrow_exception(_error);
        }

    private:
        void
        flush() {
            auto w = std::move(_waiters);
            for (auto h : w)
                schedule_via_current(h);
        }
    };

    std::shared_ptr<state> _state;
    explicit shared_task(std::shared_ptr<state> s)
        : _state(std::move(s)) {}

    static task<void>
    runner(task<void> inner, std::shared_ptr<state> s) {
        try {
            co_await std::move(inner);
            s->complete();
        } catch (...) {
            s->fail(std::current_exception());
        }
    }

public:
    shared_task()                               = default;
    shared_task(const shared_task &)            = default;
    shared_task &operator=(const shared_task &) = default;
    shared_task(shared_task &&)                 = default;
    shared_task &operator=(shared_task &&)      = default;

    [[nodiscard]] bool
    valid() const noexcept {
        return _state != nullptr;
    }
    [[nodiscard]] bool
    is_ready() const noexcept {
        return _state && _state->is_done();
    }

    struct awaiter {
        std::shared_ptr<state>  s;
        std::coroutine_handle<> _parked{}; ///< set when queued in s->_waiters

        explicit awaiter(std::shared_ptr<state> st)
            : s(std::move(st)) {}

        // Destroyed while still parked (when_any/race loser reclaim): retract our handle so a later
        // flush() cannot schedule a freed frame (the held `s` keeps the list valid here).
        ~awaiter() {
            if (!_parked || !s)
                return;
            auto &w = s->_waiters;
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (*it == _parked) {
                    w.erase(it);
                    break;
                }
            }
        }

        [[nodiscard]] bool
        await_ready() const noexcept {
            // Finding 2.A.1: same null-state guard as shared_task<T>.
            return s ? s->is_done() : false;
        }
        void
        await_suspend(std::coroutine_handle<> h) {
            if (!s) {
                throw std::logic_error("co_await on default-constructed shared_task<void>");
            }
            if (s->is_done()) {
                schedule_via_current(h);
                return;
            }
            _parked = h;
            s->_waiters.push_back(h);
        }
        void
        await_resume() {
            if (!s) {
                throw std::logic_error("co_await on default-constructed shared_task<void>");
            }
            s->get();
        }
    };

    awaiter
    operator co_await() const {
        return awaiter{_state};
    }

    friend shared_task<void> make_shared_task<void>(task<void> &&t);
};

// =============================================================================
// Factory functions
// =============================================================================

/**
 * @brief Wrap a task<T> as a shared_task<T>
 *
 * The underlying computation is spawned immediately on the current scheduler.
 * Any number of coroutines can then co_await the returned handle.
 *
 * @code
 * auto sh = make_shared_task(expensive_computation());
 * auto a  = sh;   // copy the handle
 * int v1 = co_await sh;
 * int v2 = co_await a;  // same result, no recomputation
 * @endcode
 */
template <typename T>
shared_task<T>
make_shared_task(task<T> &&t) {
    auto s = std::make_shared<typename shared_task<T>::state>();
    coro_scheduler().spawn(shared_task<T>::runner(std::move(t), s));
    return shared_task<T>{s};
}

template <>
inline shared_task<void>
make_shared_task<void>(task<void> &&t) {
    auto s = std::make_shared<typename shared_task<void>::state>();
    coro_scheduler().spawn(shared_task<void>::runner(std::move(t), s));
    return shared_task<void>{s};
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SHARED_TASK_H
