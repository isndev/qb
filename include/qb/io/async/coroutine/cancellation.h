/**
 * @file qb/io/async/coroutine/cancellation.h
 * @brief Cancellation support for coroutines
 *
 * This file provides cancellation mechanisms for coroutines:
 * - cancellation_token: Signal cancellation to operations
 * - cancellable_task: Wrapper that checks cancellation
 * - check_cancelled: Awaiter for cancellation
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

#ifndef QB_IO_ASYNC_COROUTINE_CANCELLATION_H
#define QB_IO_ASYNC_COROUTINE_CANCELLATION_H

#include "scheduler.h"
#include "task.h"
#include "utils.h"
// No <mutex> / <atomic> needed: cancellation_token is designed for same-thread
// use only (one qb-io VirtualCore thread). Cross-thread cancellation must go
// through the qb actor event system — an actor on Thread B sends a Cancel event
// to the actor on Thread A, which then calls token.cancel() on its own thread.
#include <memory>
#include <functional>
#include <vector>
#include <exception>

namespace qb::io::async {

/**
 * @brief Exception thrown when operation is cancelled
 */
class cancelled_error : public std::runtime_error {
public:
    cancelled_error() : std::runtime_error("Operation was cancelled") {}
};

// Forward declaration
class cancellation_token;

/**
 * @brief Token for coordinating cancellation across coroutines
 *
 * A cancellation_token can be shared between multiple coroutines.
 * When cancel() is called, all coroutines checking this token
 * will be notified and can gracefully exit.
 *
 * Usage:
 * @code
 * cancellation_token token;
 *
 * // Spawn worker
 * coro_scheduler().spawn(worker_task(token));
 *
 * // Later, cancel it
 * token.cancel();
 * @endcode
 */
class cancellation_token {
public:
    // Plain bool and no mutex: single-thread cooperative scheduler.
    // cancel() and on_cancel() are always called on the same VirtualCore thread.
    struct state {
        bool cancelled{false};
        std::vector<std::function<void()>> callbacks;
    };

private:
    std::shared_ptr<state> _state;

public:
    cancellation_token() : _state(std::make_shared<state>()) {}

    cancellation_token(const cancellation_token&) = default;
    cancellation_token(cancellation_token&&) = default;
    cancellation_token& operator=(const cancellation_token&) = default;
    cancellation_token& operator=(cancellation_token&&) = default;

    /**
     * @brief Cancel all operations using this token.
     *
     * Must be called on the same VirtualCore thread as the coroutines using
     * this token. For cross-thread cancellation, send a qb actor event to the
     * owning thread and call cancel() from its event handler.
     */
    void cancel() {
        if (!_state->cancelled) {
            _state->cancelled = true;
            auto callbacks = std::move(_state->callbacks);
            for (auto& cb : callbacks)
                if (cb) cb();
        }
    }

    bool is_cancelled() const noexcept { return _state->cancelled; }

    /**
     * @brief Register a callback invoked when cancel() is called.
     *
     * If already cancelled, the callback is invoked immediately (same thread).
     */
    void on_cancel(std::function<void()> callback) {
        if (_state->cancelled) {
            callback();
        } else {
            _state->callbacks.push_back(std::move(callback));
        }
    }

    void throw_if_cancelled() const {
        if (_state->cancelled) throw cancelled_error();
    }

    std::shared_ptr<state> get_state() const { return _state; }
};

/**
 * @brief Awaiter that suspends until cancellation is requested
 *
 * Usage:
 * @code
 * co_await check_cancelled(token);  // Throws if cancelled
 * @endcode
 */
struct cancellation_awaiter {
    cancellation_token token;
    std::coroutine_handle<> _handle;

    [[nodiscard]] bool await_ready() const {
        return token.is_cancelled();
    }

    void await_suspend(std::coroutine_handle<> h) {
        _handle = h;
        token.on_cancel([h]() { schedule_via_current(h); });
    }

    void await_resume() {
        token.throw_if_cancelled();
    }
};

/**
 * @brief Create awaiter that checks/waits for cancellation
 * @param token The cancellation token to check
 * @return Awaiter that throws cancelled_error when cancelled
 * @ingroup Coroutine
 */
inline cancellation_awaiter check_cancelled(const cancellation_token& token) {
    return cancellation_awaiter{token, std::coroutine_handle<>{}};
}

/**
 * @brief Awaiter that yields control back to scheduler
 * Checks cancellation each time it resumes
 *
 * Single-thread: enqueues the current handle at the end of the ready queue
 * without allocating an intermediate coroutine.
 */
struct yield_awaiter {
    cancellation_token token;

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        enqueue_for_later_via_current(h);
    }

    void await_resume() {
        token.throw_if_cancelled();
    }
};

/**
 * @brief Yield control and check cancellation
 * @param token Cancellation token to check
 * @return Awaiter that yields and throws if cancelled
 * @ingroup Coroutine
 */
inline yield_awaiter yield_or_cancel(const cancellation_token& token) {
    return yield_awaiter{token};
}

/**
 * @brief Wrapper for cancellable operations with automatic cleanup
 * @tparam T Return type of the wrapped task
 *
 * Stores the inner task and shared state in a shared_ptr so that the spawned
 * task_runner coroutine (a static function) can safely access it even after the
 * outer coroutine resumes via cancellation and the awaiter is destroyed.
 * Never use a [this] or [&] lambda as a coroutine body — the compiler stores
 * a pointer to the local lambda, not a copy, causing dangling access on resume.
 */
template <typename T>
class cancellable_operation {
    struct shared_state {
        task<T> inner_task;
        std::optional<T> result;
        bool task_done{false};  // single-thread: no atomic needed
        std::coroutine_handle<> continuation;

        explicit shared_state(task<T>&& t) : inner_task(std::move(t)) {}
    };

    std::shared_ptr<shared_state> _shared;
    cancellation_token _token;
    bool _throw_on_cancel;

public:
    cancellable_operation(task<T>&& t, cancellation_token token, bool throw_on_cancel = true)
        : _shared(std::make_shared<shared_state>(std::move(t)))
        , _token(std::move(token))
        , _throw_on_cancel(throw_on_cancel) {}

    struct awaiter {
        std::shared_ptr<shared_state> state;
        cancellation_token token;
        bool throw_on_cancel;

        [[nodiscard]] bool await_ready() const {
            return token.is_cancelled() && throw_on_cancel;
        }

        void await_suspend(std::coroutine_handle<> h) {
            state->continuation = h;
            coro_scheduler().spawn(task_runner(state));
            if (throw_on_cancel)
                token.on_cancel([h]() { schedule_via_current(h); });
        }

        T await_resume() {
            if (token.is_cancelled() && throw_on_cancel) {
                throw cancelled_error();
            }
            if (state->result) return std::move(*state->result);
            return T{};
        }

    private:
        static task<void> task_runner(std::shared_ptr<shared_state> state) {
            try {
                state->result = co_await state->inner_task;
            } catch (...) {}
            if (!state->task_done) {
                state->task_done = true;
                if (state->continuation) schedule_via_current(state->continuation);
            }
        }
    };

    awaiter operator co_await() {
        return awaiter{_shared, _token, _throw_on_cancel};
    }
};

// Specialization for void
template <>
class cancellable_operation<void> {
    struct shared_state {
        task<void> inner_task;
        bool task_done{false};  // single-thread
        std::coroutine_handle<> continuation;

        explicit shared_state(task<void>&& t) : inner_task(std::move(t)) {}
    };

    std::shared_ptr<shared_state> _shared;
    cancellation_token _token;
    bool _throw_on_cancel;

public:
    cancellable_operation(task<void>&& t, cancellation_token token, bool throw_on_cancel = true)
        : _shared(std::make_shared<shared_state>(std::move(t)))
        , _token(std::move(token))
        , _throw_on_cancel(throw_on_cancel) {}

    struct awaiter {
        std::shared_ptr<shared_state> state;
        cancellation_token token;
        bool throw_on_cancel;

        [[nodiscard]] bool await_ready() const {
            return token.is_cancelled() && throw_on_cancel;
        }

        void await_suspend(std::coroutine_handle<> h) {
            state->continuation = h;
            coro_scheduler().spawn(task_runner(state));
            if (throw_on_cancel)
                token.on_cancel([h]() { schedule_via_current(h); });
        }

        void await_resume() {
            if (token.is_cancelled() && throw_on_cancel)
                throw cancelled_error();
        }

    private:
        static task<void> task_runner(std::shared_ptr<shared_state> state) {
            try {
                co_await state->inner_task;
            } catch (...) {}
            if (!state->task_done) {
                state->task_done = true;
                if (state->continuation) schedule_via_current(state->continuation);
            }
        }
    };

    awaiter operator co_await() {
        return awaiter{_shared, _token, _throw_on_cancel};
    }
};

/**
 * @brief Wrap a task with cancellation support
 * @param task Task to wrap
 * @param token Cancellation token
 * @param throw_on_cancel Whether to throw on cancellation
 * @return Cancellable operation wrapper
 * @ingroup Coroutine
 */
template <typename T>
auto make_cancellable(task<T>&& task, cancellation_token token, bool throw_on_cancel = true) {
    return cancellable_operation<T>(std::move(task), std::move(token), throw_on_cancel);
}

/**
 * @brief Awaiter for sleep with immediate wake on cancellation
 *
 * Registers the coroutine handle with the token so that cancel() schedules
 * it immediately instead of polling every 10ms.
 */
/**
 * @brief Awaiter for sleep with immediate wake on cancellation.
 *
 * Uses a shared_ptr<sleep_state> and a static free function (not a lambda) for the
 * timer coroutine, so the coroutine frame captures its arguments by value as function
 * parameters — not via a lambda closure that could dangle when await_suspend returns.
 */
struct cancellable_sleep_awaiter {
    // Plain bool: single-thread cooperative — the on_cancel callback and the
    // timer_task coroutine run on the same thread and never concurrently.
    struct sleep_state {
        bool resumed{false};
        std::coroutine_handle<> handle;
    };

    std::chrono::milliseconds duration;
    cancellation_token token;

    [[nodiscard]] bool await_ready() const { return token.is_cancelled(); }

    void await_suspend(std::coroutine_handle<> h) {
        auto state = std::make_shared<sleep_state>();
        state->handle = h;
        token.on_cancel([state]() {
            if (!state->resumed) {
                state->resumed = true;
                schedule_via_current(state->handle);
            }
        });
        coro_scheduler().spawn(timer_task(duration, std::move(state)));
    }

    void await_resume() { token.throw_if_cancelled(); }

private:
    // Static function: d and s are VALUE parameters stored in the coroutine frame.
    // Never use a lambda here — the compiler may store a pointer to the local lambda
    // object rather than a copy, causing a dangling reference after await_suspend returns.
    static task<void> timer_task(std::chrono::milliseconds d, std::shared_ptr<sleep_state> s) {
        co_await sleep(d);
        if (!s->resumed) {
            s->resumed = true;
            schedule_via_current(s->handle);
        }
    }
};

/**
 * @brief Sleep that can be cancelled; wakes immediately when token is cancelled
 * @param duration Sleep duration
 * @param token Cancellation token
 * @return Task that completes after duration or when cancelled
 * @ingroup Coroutine
 */
inline task<void> cancellable_sleep(std::chrono::milliseconds duration, cancellation_token token) {
    co_await cancellable_sleep_awaiter{duration, std::move(token)};
}

namespace detail {
struct with_deadline_timeout_state {
    bool completed{false};  // single-thread
    int result{0};
    std::coroutine_handle<> handle;
};

struct with_deadline_timeout_awaiter {
    std::shared_ptr<with_deadline_timeout_state> state;
    std::chrono::steady_clock::time_point deadline;
    cancellation_token token;

    [[nodiscard]] bool await_ready() const {
        if (token.is_cancelled()) {
            state->result = 1;
            state->completed = true;
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        state->handle = h;
        token.on_cancel([s = state]() {
            if (!s->completed) {
                s->completed = true;
                s->result = 1;
                schedule_via_current(s->handle);
            }
        });

        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) {
            if (!state->completed) {
                state->completed = true;
                state->result = 0;
                schedule_via_current(h);
            }
            return;
        }

        coro_scheduler().spawn(deadline_timer_task(state, remaining));
    }

    int await_resume() const { return state->result; }

private:
    static task<void> deadline_timer_task(
        std::shared_ptr<with_deadline_timeout_state> s,
        std::chrono::milliseconds remaining)
    {
        co_await sleep(remaining);
        if (!s->completed) {
            s->completed = true;
            s->result = 0;
            schedule_via_current(s->handle);
        }
    }
};

// Free function: s, dl, tok are VALUE parameters in the coroutine frame.
// Must NOT be a lambda defined inside with_deadline — the lambda would be a
// local variable in with_deadline's frame and the compiler may store only a
// pointer to it in the spawned coroutine frame. If with_deadline's frame is
// destroyed first (e.g. after throwing timeout_error), that pointer dangles.
inline task<int> with_deadline_run_timeout(
    std::shared_ptr<with_deadline_timeout_state> s,
    std::chrono::steady_clock::time_point dl,
    cancellation_token tok)
{
    co_return co_await with_deadline_timeout_awaiter{std::move(s), dl, std::move(tok)};
}

} // namespace detail

/**
 * @brief Helper to run operation with deadline
 *
 * Single sleep until deadline (no 10ms polling). Cancel wakes immediately via token callback.
 *
 * @param operation Task to run
 * @param deadline Max time point
 * @param token Cancellation token (optional)
 * @return Result or throws timeout_error / cancelled_error
 * @ingroup Coroutine
 */
template <typename T>
task<T> with_deadline(task<T>&& operation, std::chrono::steady_clock::time_point deadline,
                      cancellation_token token = {}) {
    // Do not throw before co_await: can cause use-after-free when the caller awaits.
    auto state = std::make_shared<detail::with_deadline_timeout_state>();

    // Use a free function (not a lambda) to avoid the dangling-lambda-pointer bug:
    // if we used a local lambda here, with_deadline's frame might be destroyed
    // (after throwing timeout_error) while the spawned timeout coroutine still
    // holds a pointer to that local lambda object.
    auto res = co_await when_any(std::move(operation),
                                 detail::with_deadline_run_timeout(state, deadline, token));

    if (res.index == 0) {
        // Operation finished first; if deadline already passed, treat as timeout
        if (std::chrono::steady_clock::now() >= deadline) {
            throw timeout_error();
        }
        co_return res.template get<T>();
    }
    if (res.template get<int>() == 1) {
        throw cancelled_error();
    }
    throw timeout_error();
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CANCELLATION_H
