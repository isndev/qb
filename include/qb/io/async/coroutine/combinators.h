/**
 * @file qb/io/async/coroutine/combinators.h
 * @brief Coroutine combinators for parallel execution
 *
 * This file provides combinator functions for composing multiple coroutines:
 * - when_all: Wait for all coroutines to complete
 * - when_any: Wait for the first coroutine to complete
 * - race: Wait for first, cancel others
 * - coro_with_timeout: Execute with timeout limit
 *
 * Single-thread: all composed tasks run on the same thread; use one scheduler per thread.
 *
 * LIFETIME DESIGN:
 * ================
 * All variadic/vector awaiters store their mutable shared state in a shared_ptr<state_t>.
 * The spawned coroutines capture this shared_ptr by value, so the state outlives the
 * awaiter object itself (which is destroyed when the outer coroutine resumes past the
 * co_await). Without this pattern, the [this] captures inside spawned coroutines become
 * dangling once the first winner fires and the awaiter is destroyed.
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

#ifndef QB_IO_ASYNC_COROUTINE_COMBINATORS_H
#define QB_IO_ASYNC_COROUTINE_COMBINATORS_H

#include "task.h"
#include "utils.h"
#include <any>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace qb::io::async {

// =============================================================================
// when_all - Wait for all coroutines
// =============================================================================

/**
 * @brief Awaiter that waits for all tasks to complete (variadic version)
 * @tparam Tasks Types of tasks to wait for
 *
 * Uses shared_ptr<state_t> so the spawned coroutines safely access the
 * shared state even after the awaiter object is destroyed when the outer
 * coroutine resumes.
 *
 * Usage:
 * @code
 * auto [r1, r2, r3] = co_await when_all(task1(), task2(), task3());
 * @endcode
 */
template <typename... Tasks>
class when_all_awaiter {
public:
    using result_type = std::tuple<typename Tasks::value_type...>;
    static constexpr size_t N = sizeof...(Tasks);

private:
    struct state_t {
        std::tuple<Tasks...> tasks;
        result_type results{};
        // Plain size_t: single-thread cooperative — run_one coroutines never
        // execute concurrently, so incrementing is always sequential.
        size_t completed{0};
        std::coroutine_handle<> continuation;
        std::exception_ptr first_exception;

        explicit state_t(Tasks... ts) : tasks(std::move(ts)...) {}
    };

    std::shared_ptr<state_t> _state;

    template <size_t I>
    static task<void> run_one(std::shared_ptr<state_t> state) {
        try {
            std::get<I>(state->results) = co_await std::get<I>(state->tasks);
        } catch (...) {
            if (!state->first_exception)
                state->first_exception = std::current_exception();
        }
        if (++state->completed == N) {
            if (state->continuation)
                schedule_via_current(state->continuation);
        }
    }

    template <size_t... Is>
    void start_all(std::index_sequence<Is...>) {
        (coro_scheduler().spawn(run_one<Is>(_state)), ...);
    }

public:
    explicit when_all_awaiter(Tasks... tasks)
        : _state(std::make_shared<state_t>(std::move(tasks)...)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return N == 0;
    }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        start_all(std::make_index_sequence<N>{});
    }

    result_type await_resume() {
        if (_state->first_exception) {
            std::rethrow_exception(_state->first_exception);
        }
        return std::move(_state->results);
    }
};

/**
 * @brief Wait for all coroutines to complete
 * @param tasks Tasks to wait for
 * @return Awaiter that returns tuple of all results
 * @ingroup Coroutine
 */
template <typename... Tasks>
auto when_all(Tasks... tasks) {
    return when_all_awaiter<Tasks...>(std::move(tasks)...);
}

// when_all(vector<task<T>>): shared state + free function run_one so the index
// is a function parameter (coroutine frame), not a lambda capture that could
// dangle when the awaiter is destroyed while spawned coroutines are still running.
template <typename T>
class when_all_vector_awaiter {
public:
    using result_type = std::vector<T>;

    explicit when_all_vector_awaiter(std::vector<task<T>> tasks)
        : _state(std::make_shared<state_t>(std::move(tasks))) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return _state->tasks.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        const size_t n = _state->tasks.size();
        std::shared_ptr<state_t> state = _state;
        for (size_t i = 0; i < n; ++i) {
            coro_scheduler().spawn(run_one(state, i, n));
        }
    }

    result_type await_resume() {
        if (_state->first_exception) {
            std::rethrow_exception(_state->first_exception);
        }
        return std::move(_state->results);
    }

private:
    struct state_t {
        std::vector<task<T>> tasks;
        std::vector<T> results;
        size_t completed{0};  // single-thread
        std::coroutine_handle<> continuation;
        std::exception_ptr first_exception;
        explicit state_t(std::vector<task<T>> t)
            : tasks(std::move(t))
            , results(this->tasks.size()) {}
    };
    std::shared_ptr<state_t> _state;

    static task<void> run_one(std::shared_ptr<state_t> state, size_t i, size_t n) {
        try {
            state->results[i] = co_await state->tasks[i];
        } catch (...) {
            if (!state->first_exception)
                state->first_exception = std::current_exception();
        }
        if (++state->completed == n) {
            if (state->continuation)
                schedule_via_current(state->continuation);
        }
    }
};

/**
 * @brief Wait for all coroutines in a vector to complete
 * @param tasks Vector of tasks
 * @return Awaiter that returns vector of results
 * @ingroup Coroutine
 */
template <typename T>
auto when_all(std::vector<task<T>> tasks) {
    return when_all_vector_awaiter<T>(std::move(tasks));
}

// =============================================================================
// when_any - Wait for first coroutine
// =============================================================================

/**
 * @brief Result of when_any - index and value of first completed task
 * Uses std::any to avoid variant with duplicate types
 */
struct when_any_result {
    size_t index;
    std::any value;
    std::exception_ptr exception; ///< Non-null when the winning task threw

    template <typename T>
    T get() const {
        if (exception)
            std::rethrow_exception(exception);
        return std::any_cast<T>(value);
    }

    /// @brief Check whether the winning task completed with an exception
    [[nodiscard]] bool has_exception() const noexcept { return exception != nullptr; }
};

// Structured binding: auto [index, value] = co_await when_any(...);
template <size_t I>
inline constexpr decltype(auto) get(const when_any_result& r) noexcept {
    if constexpr (I == 0) return r.index;
    else return static_cast<const std::any&>(r.value);
}

} // namespace qb::io::async

namespace std {
template <>
struct tuple_size<qb::io::async::when_any_result> : integral_constant<size_t, 2> {};

template <size_t I>
struct tuple_element<I, qb::io::async::when_any_result> {};

template <>
struct tuple_element<0, qb::io::async::when_any_result> {
    using type = size_t;
};

template <>
struct tuple_element<1, qb::io::async::when_any_result> {
    using type = const any;
};
} // namespace std

namespace qb::io::async::detail {
// Finding 2.B.14: tuple_size<> exposes index + value as the structured-binding
// surface of when_any_result. The struct also carries a non-binding
// `exception` field (accessed via `get<T>()`), which is intentionally NOT
// exposed via tuple_element. This static_assert locks the arity so an
// accidental addition of a third binding field breaks the build instead of
// silently shifting existing bindings.
static_assert(std::tuple_size<when_any_result>::value == 2,
              "when_any_result must expose exactly (index, value) for "
              "structured bindings; adjust tuple_element specialisations too");
}

namespace qb::io::async {

/**
 * @brief Awaiter that waits for the first task to complete (variadic version)
 * @tparam Tasks Types of tasks
 *
 * Uses shared_ptr<state_t> so the losing spawned coroutines can safely
 * no-op after the outer coroutine has already resumed and destroyed the
 * awaiter object.
 */
template <typename... Tasks>
class when_any_awaiter {
public:
    using result_type = when_any_result;
    static constexpr size_t N = sizeof...(Tasks);

private:
    struct state_t {
        std::tuple<Tasks...> tasks;
        std::optional<when_any_result> result;
        // Plain bool: only one run_one can be executing at a time (cooperative).
        bool done{false};
        std::coroutine_handle<> continuation;

        explicit state_t(Tasks... ts) : tasks(std::move(ts)...) {}
    };

    std::shared_ptr<state_t> _state;

    template <size_t I>
    static task<void> run_one(std::shared_ptr<state_t> state) {
        using task_type = std::tuple_element_t<I, std::tuple<Tasks...>>;
        using value_type = typename task_type::value_type;
        try {
            std::any stored;
            if constexpr (std::is_void_v<value_type>) {
                co_await std::get<I>(state->tasks);
            } else {
                stored = std::any(co_await std::get<I>(state->tasks));
            }
            if (!state->done) {
                state->done = true;
                state->result = when_any_result{
                    I,
                    std::is_void_v<value_type> ? std::any{} : std::move(stored),
                    nullptr
                };
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        } catch (...) {
            if (!state->done) {
                state->done = true;
                state->result = when_any_result{I, std::any{}, std::current_exception()};
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        }
    }

    template <size_t... Is>
    void start_all(std::index_sequence<Is...>) {
        (coro_scheduler().spawn(run_one<Is>(_state)), ...);
    }

public:
    explicit when_any_awaiter(Tasks... tasks)
        : _state(std::make_shared<state_t>(std::move(tasks)...)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return N == 0;
    }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        start_all(std::make_index_sequence<N>{});
    }

    result_type await_resume() {
        if (!_state->result) {
            return when_any_result{0, std::any{}, nullptr};
        }
        return std::move(*_state->result);
    }
};

/**
 * @brief Wait for the first coroutine to complete
 * @param tasks Tasks to race
 * @return Awaiter that returns index and value of first completed
 * @ingroup Coroutine
 */
template <typename... Tasks>
auto when_any(Tasks... tasks) {
    return when_any_awaiter<Tasks...>(std::move(tasks)...);
}

// Vector version: same lifetime rule as when_all_vector — shared state and
// a free function with explicit index parameter stored in the coroutine frame.
template <typename T>
class when_any_vector_awaiter {
public:
    using result_type = std::pair<size_t, std::any>;

    explicit when_any_vector_awaiter(std::vector<task<T>> tasks)
        : _state(std::make_shared<state_t>(std::move(tasks))) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return _state->tasks.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        const size_t n = _state->tasks.size();
        std::shared_ptr<state_t> state = _state;
        for (size_t i = 0; i < n; ++i) {
            coro_scheduler().spawn(run_one(state, i, n));
        }
    }

    result_type await_resume() {
        if (_state->exception)
            std::rethrow_exception(_state->exception);
        if (!_state->result)
            return result_type{0, std::any{}};  // empty vector edge case
        return std::move(*_state->result);
    }

private:
    struct state_t {
        std::vector<task<T>> tasks;
        std::optional<result_type> result;
        std::exception_ptr exception;
        bool done{false};  // single-thread
        std::coroutine_handle<> continuation;
        explicit state_t(std::vector<task<T>> t) : tasks(std::move(t)) {}
    };
    std::shared_ptr<state_t> _state;

    static task<void> run_one(std::shared_ptr<state_t> state, size_t i, size_t n) {
        (void)n;
        try {
            auto value = co_await state->tasks[i];
            if (!state->done) {
                state->done = true;
                state->result = result_type{i, std::any(std::move(value))};
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        } catch (...) {
            if (!state->done) {
                state->done = true;
                state->exception = std::current_exception();
                state->result = result_type{i, std::any{}};
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        }
    }
};

/**
 * @brief Wait for first coroutine in vector to complete
 * @param tasks Vector of tasks
 * @return Awaiter that returns index and value of first completed
 * @ingroup Coroutine
 */
template <typename T>
auto when_any(std::vector<task<T>> tasks) {
    return when_any_vector_awaiter<T>(std::move(tasks));
}

// =============================================================================
// coro_with_timeout - Execute with timeout
// =============================================================================

/**
 * @brief Exception thrown when operation times out
 */
class timeout_error : public std::runtime_error {
public:
    timeout_error() : std::runtime_error("Operation timed out") {}
};

/**
 * @brief Awaiter that adds timeout to any operation (non-void version)
 * @tparam T Result type of the task
 *
 * Uses shared_ptr<state_t> so both the task coroutine and the timeout
 * coroutine can safely access shared state after the awaiter is destroyed
 * when the outer coroutine resumes (the loser still runs to completion
 * but its compare_exchange will fail and it simply exits).
 */
template <typename T>
class timeout_awaiter {
    struct state_t {
        task<T> inner_task;
        std::optional<T> result;
        std::exception_ptr exception;
        // Plain bool: run_task and run_timeout run cooperatively on the same
        // thread — only one can be executing at a time, so the "first wins"
        // check is always sequential.
        bool completed{false};
        bool timed_out{false};
        std::coroutine_handle<> continuation;

        explicit state_t(task<T>&& t) : inner_task(std::move(t)) {}
    };

    std::shared_ptr<state_t> _state;
    std::chrono::milliseconds _timeout;

    static task<void> run_task(std::shared_ptr<state_t> state) {
        try {
            state->result = co_await state->inner_task;
        } catch (...) {
            state->exception = std::current_exception();
        }
        if (!state->completed) {
            state->completed = true;
            if (state->continuation)
                schedule_via_current(state->continuation);
        }
    }

    static task<void> run_timeout(std::shared_ptr<state_t> state, std::chrono::milliseconds timeout) {
        co_await sleep(timeout);
        if (!state->completed) {
            state->completed = true;
            state->timed_out = true;
            if (state->continuation)
                schedule_via_current(state->continuation);
        }
    }

public:
    timeout_awaiter(task<T>&& t, std::chrono::milliseconds timeout)
        : _state(std::make_shared<state_t>(std::move(t)))
        , _timeout(timeout) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        coro_scheduler().spawn(run_task(_state));
        coro_scheduler().spawn(run_timeout(_state, _timeout));
    }

    T await_resume() {
        if (_state->timed_out) {
            throw timeout_error();
        }
        if (_state->exception) {
            std::rethrow_exception(_state->exception);
        }
        return std::move(*_state->result);
    }
};

/**
 * @brief Execute a coroutine with a timeout
 * @param t The task to execute
 * @param timeout Maximum time to wait
 * @return Awaiter that returns task result or throws timeout_error
 * @throws timeout_error if operation exceeds timeout
 *
 * Finding 2.B.4 — cancellation semantics:
 *   When the timeout fires before the task completes, the awaiter throws
 *   `timeout_error` to the caller but **does not interrupt** the inner
 *   task: it keeps running in the background until it finishes naturally
 *   (its result is then dropped). If you need cooperative interruption,
 *   compose with `with_deadline` and a `cancellation_token` instead.
 *
 * @ingroup Coroutine
 */
template <typename T>
auto coro_with_timeout(task<T>&& t, std::chrono::milliseconds timeout) {
    return timeout_awaiter<T>(std::move(t), timeout);
}

/**
 * @brief Specialization for void tasks
 */
template <>
class timeout_awaiter<void> {
    struct state_t {
        task<void> inner_task;
        std::exception_ptr exception;
        bool completed{false};  // single-thread
        bool timed_out{false};
        std::coroutine_handle<> continuation;

        explicit state_t(task<void>&& t) : inner_task(std::move(t)) {}
    };

    std::shared_ptr<state_t> _state;
    std::chrono::milliseconds _timeout;

    static task<void> run_task(std::shared_ptr<state_t> state) {
        try {
            co_await state->inner_task;
        } catch (...) {
            state->exception = std::current_exception();
        }
        if (!state->completed) {
            state->completed = true;
            if (state->continuation) schedule_via_current(state->continuation);
        }
    }

    static task<void> run_timeout(std::shared_ptr<state_t> state, std::chrono::milliseconds timeout) {
        co_await sleep(timeout);
        if (!state->completed) {
            state->completed = true;
            state->timed_out = true;
            if (state->continuation) schedule_via_current(state->continuation);
        }
    }

public:
    timeout_awaiter(task<void>&& t, std::chrono::milliseconds timeout)
        : _state(std::make_shared<state_t>(std::move(t)))
        , _timeout(timeout) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        coro_scheduler().spawn(run_task(_state));
        coro_scheduler().spawn(run_timeout(_state, _timeout));
    }

    void await_resume() {
        if (_state->timed_out) {
            throw timeout_error();
        }
        if (_state->exception) {
            std::rethrow_exception(_state->exception);
        }
    }
};

/**
 * @brief Helper to create timeout awaiter (void specialization)
 * @ingroup Coroutine
 */
inline auto coro_with_timeout(task<void>&& t, std::chrono::milliseconds timeout) {
    return timeout_awaiter<void>(std::move(t), timeout);
}

// =============================================================================
// Race - Wait for first, implicit cancellation of others
// =============================================================================

/**
 * @brief Race multiple tasks, return first result
 *
 * Semantic alias for `when_any` (first winner wins). The losers are **not**
 * cancelled: they keep running to completion in the background, owned by
 * the scheduler. If you need true cancellation of the losers, wrap each
 * task in a `cancellable_operation` and call `cancel()` on the tokens from
 * the winner's continuation (finding 2.B.3).
 *
 * @ingroup Coroutine
 */
template <typename... Tasks>
auto race(Tasks... tasks) {
    return when_any(std::move(tasks)...);
}

/**
 * @brief Race vector of tasks
 *
 * See `race(Tasks...)`. Same non-cancelling semantics (finding 2.B.3).
 * @ingroup Coroutine
 */
template <typename T>
auto race(std::vector<task<T>> tasks) {
    return when_any(std::move(tasks));
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_COMBINATORS_H
