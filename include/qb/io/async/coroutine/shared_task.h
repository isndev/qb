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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0
 */

#pragma once
#ifndef QB_IO_ASYNC_COROUTINE_SHARED_TASK_H
#define QB_IO_ASYNC_COROUTINE_SHARED_TASK_H

#include <any>
#include <exception>
#include <memory>
#include <vector>
#include "task.h"
#include "scheduler.h"

namespace qb::io::async {

// Forward declaration
template <typename T> class shared_task;
template <typename T> shared_task<T> make_shared_task(task<T>&& t);

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

        status             _status{status::pending};
        T                  _value{};
        std::exception_ptr _error;
        // All coroutines waiting for the result
        std::vector<std::coroutine_handle<>> _waiters;

        void complete(T val) {
            _value  = std::move(val);
            _status = status::ready;
            flush();
        }

        void fail(std::exception_ptr e) {
            _error  = e;
            _status = status::failed;
            flush();
        }

        bool is_done() const noexcept {
            return _status != status::pending;
        }

        T get() const {
            if (_status == status::failed) std::rethrow_exception(_error);
            return _value;
        }

    private:
        void flush() {
            auto w = std::move(_waiters);
            for (auto h : w) schedule_via_current(h);
        }
    };

    std::shared_ptr<state> _state;

    explicit shared_task(std::shared_ptr<state> s) : _state(std::move(s)) {}

    // The runner coroutine: runs the inner task, then notifies all waiters.
    // Parameters are stored by value in the coroutine frame (safe).
    static task<void> runner(task<T> inner, std::shared_ptr<state> s) {
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
    shared_task(const shared_task&)            = default;
    shared_task& operator=(const shared_task&) = default;
    shared_task(shared_task&&)                 = default;
    shared_task& operator=(shared_task&&)      = default;

    [[nodiscard]] bool valid()    const noexcept { return _state != nullptr; }
    [[nodiscard]] bool is_ready() const noexcept { return _state && _state->is_done(); }

    // -----------------------------------------------------------------------
    // Awaiter
    // -----------------------------------------------------------------------
    struct awaiter {
        std::shared_ptr<state> s;

        [[nodiscard]] bool await_ready() const noexcept { return s->is_done(); }

        void await_suspend(std::coroutine_handle<> h) {
            if (s->is_done()) schedule_via_current(h);
            else              s->_waiters.push_back(h);
        }

        T await_resume() { return s->get(); }
    };

    awaiter operator co_await() const {
        return awaiter{_state};
    }

    friend shared_task<T> make_shared_task<T>(task<T>&& t);
};

// =============================================================================
// shared_task<void> specialisation
// =============================================================================

template <>
class shared_task<void> {
    struct state {
        enum class status { pending, ready, failed };
        status             _status{status::pending};
        std::exception_ptr _error;
        std::vector<std::coroutine_handle<>> _waiters;

        void complete() {
            _status = status::ready;
            flush();
        }
        void fail(std::exception_ptr e) {
            _error  = e;
            _status = status::failed;
            flush();
        }
        bool is_done() const noexcept { return _status != status::pending; }
        void get() const { if (_status == status::failed) std::rethrow_exception(_error); }

    private:
        void flush() {
            auto w = std::move(_waiters);
            for (auto h : w) schedule_via_current(h);
        }
    };

    std::shared_ptr<state> _state;
    explicit shared_task(std::shared_ptr<state> s) : _state(std::move(s)) {}

    static task<void> runner(task<void> inner, std::shared_ptr<state> s) {
        try {
            co_await std::move(inner);
            s->complete();
        } catch (...) {
            s->fail(std::current_exception());
        }
    }

public:
    shared_task() = default;
    shared_task(const shared_task&)            = default;
    shared_task& operator=(const shared_task&) = default;
    shared_task(shared_task&&)                 = default;
    shared_task& operator=(shared_task&&)      = default;

    [[nodiscard]] bool valid()    const noexcept { return _state != nullptr; }
    [[nodiscard]] bool is_ready() const noexcept { return _state && _state->is_done(); }

    struct awaiter {
        std::shared_ptr<state> s;
        [[nodiscard]] bool await_ready() const noexcept { return s->is_done(); }
        void await_suspend(std::coroutine_handle<> h) {
            if (s->is_done()) schedule_via_current(h);
            else              s->_waiters.push_back(h);
        }
        void await_resume() { s->get(); }
    };

    awaiter operator co_await() const { return awaiter{_state}; }

    friend shared_task<void> make_shared_task<void>(task<void>&& t);
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
shared_task<T> make_shared_task(task<T>&& t) {
    auto s = std::make_shared<typename shared_task<T>::state>();
    coro_scheduler().spawn(shared_task<T>::runner(std::move(t), s));
    return shared_task<T>{s};
}

template <>
inline shared_task<void> make_shared_task<void>(task<void>&& t) {
    auto s = std::make_shared<typename shared_task<void>::state>();
    coro_scheduler().spawn(shared_task<void>::runner(std::move(t), s));
    return shared_task<void>{s};
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SHARED_TASK_H
