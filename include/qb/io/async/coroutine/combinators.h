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

#ifndef QB_IO_ASYNC_COROUTINE_COMBINATORS_H
#define QB_IO_ASYNC_COROUTINE_COMBINATORS_H

#include "task.h"
#include "utils.h"
#include <qb/system/time.h> // qb::duration
#include <any>
#include <array>
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
    using result_type         = std::tuple<typename Tasks::value_type...>;
    static constexpr size_t N = sizeof...(Tasks);

private:
    struct state_t {
        std::tuple<Tasks...> tasks;
        result_type          results{};
        // Plain size_t: single-thread cooperative — run_one coroutines never
        // execute concurrently, so incrementing is always sequential.
        size_t                  completed{0};
        std::coroutine_handle<> continuation;
        std::exception_ptr      first_exception;

        explicit state_t(Tasks... ts)
            : tasks(std::move(ts)...) {}
    };

    std::shared_ptr<state_t> _state;

    template <size_t I>
    static task<void>
    run_one(std::shared_ptr<state_t> state) {
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
    void
    start_all(std::index_sequence<Is...>) {
        (coro_scheduler().spawn(run_one<Is>(_state)), ...);
    }

public:
    explicit when_all_awaiter(Tasks... tasks)
        : _state(std::make_shared<state_t>(std::move(tasks)...)) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return N == 0;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        start_all(std::make_index_sequence<N>{});
    }

    result_type
    await_resume() {
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
auto
when_all(Tasks... tasks) {
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

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->tasks.empty();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation           = h;
        const size_t             n     = _state->tasks.size();
        std::shared_ptr<state_t> state = _state;
        for (size_t i = 0; i < n; ++i) {
            coro_scheduler().spawn(run_one(state, i, n));
        }
    }

    result_type
    await_resume() {
        if (_state->first_exception) {
            std::rethrow_exception(_state->first_exception);
        }
        return std::move(_state->results);
    }

private:
    struct state_t {
        std::vector<task<T>>    tasks;
        std::vector<T>          results;
        size_t                  completed{0}; // single-thread
        std::coroutine_handle<> continuation;
        std::exception_ptr      first_exception;
        explicit state_t(std::vector<task<T>> t)
            : tasks(std::move(t))
            , results(this->tasks.size()) {}
    };
    std::shared_ptr<state_t> _state;

    static task<void>
    run_one(std::shared_ptr<state_t> state, size_t i, size_t n) {
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
auto
when_all(std::vector<task<T>> tasks) {
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
    size_t             index;
    std::any           value;
    std::exception_ptr exception; ///< Non-null when the winning task threw

    template <typename T>
    T
    get() const {
        if (exception)
            std::rethrow_exception(exception);
        return std::any_cast<T>(value);
    }

    /// @brief Check whether the winning task completed with an exception
    [[nodiscard]] bool
    has_exception() const noexcept {
        return exception != nullptr;
    }
};

// Structured binding: auto [index, value] = co_await when_any(...);
template <size_t I>
inline constexpr decltype(auto)
get(const when_any_result &r) noexcept {
    if constexpr (I == 0)
        return r.index;
    else
        return static_cast<const std::any &>(r.value);
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
static_assert(std::tuple_size<when_any_result>::value == 2, "when_any_result must expose exactly (index, value) for "
                                                            "structured bindings; adjust tuple_element specialisations too");
} // namespace qb::io::async::detail

namespace qb::io::async {

/**
 * @brief Awaiter that waits for the first task to complete (variadic version)
 * @tparam Tasks Types of tasks
 *
 * Uses shared_ptr<state_t> so the shared race state outlives the awaiter object
 * (destroyed when the outer coroutine resumes past the co_await). When a branch
 * wins it reclaims every losing branch — each loser's spawned run_one frame and
 * the inner task it was parked on are destroyed, stopping any libev watcher via
 * the awaiter destructor rather than leaving the loser to run detached. If the
 * awaiter itself is destroyed while still racing (the awaiting coroutine was
 * unwound before any winner), the destructor tears every branch down.
 */
template <typename... Tasks>
class when_any_awaiter {
public:
    using result_type         = when_any_result;
    static constexpr size_t N = sizeof...(Tasks);

private:
    struct state_t {
        std::tuple<Tasks...>           tasks;
        std::optional<when_any_result> result;
        // Plain bool: only one run_one can be executing at a time (cooperative).
        bool                    done{false};
        std::coroutine_handle<> continuation;
        // The spawned run_one frame for each branch, recorded so the winner (and the
        // pending-teardown destructor) can reclaim every losing branch instead of
        // letting it run detached to completion. A slot is cleared as its branch is
        // reclaimed (or by the winner for itself, which self-reclaims at final_suspend).
        std::array<std::coroutine_handle<>, N> branch_handles{};

        explicit state_t(Tasks... ts)
            : tasks(std::move(ts)...) {}
    };

    std::shared_ptr<state_t> _state;

    // Reclaim one losing branch: destroy its spawned run_one frame (owned — freed via
    // cancel_spawned) and the inner task<T> it was parked on (owned by state->tasks —
    // scrubbed from the scheduler queues via forget(), then destroyed by the task<T>
    // dtor, which stops any libev watcher through the inner awaiter's dtor). Order
    // mirrors the cancellable_operation fix: destroy the runner first so the inner
    // task's continuation_ (→ the runner frame) can never be resumed, then drop the
    // inner task. No-op for an already-cleared slot (the winner, or a reclaimed loser).
    template <size_t I>
    static void
    reclaim_branch(state_t &st) {
        if (!st.branch_handles[I])
            return;
        auto &sched = coro_scheduler();
        sched.cancel_spawned(std::exchange(st.branch_handles[I], {}));
        if (auto inner = std::get<I>(st.tasks).handle())
            sched.forget(inner);
        std::get<I>(st.tasks) = std::tuple_element_t<I, std::tuple<Tasks...>>{};
    }

    // The winning branch is currently executing (resumed by its own inner task) and
    // self-reclaims at final_suspend — clear its slot first so reclaim_branch<Winner>
    // is a no-op, then reclaim every other branch.
    template <size_t Winner, size_t... Is>
    static void
    reclaim_losers_impl(state_t &st, std::index_sequence<Is...>) {
        st.branch_handles[Winner] = {};
        (reclaim_branch<Is>(st), ...);
    }
    template <size_t Winner>
    static void
    reclaim_losers(state_t &st) {
        reclaim_losers_impl<Winner>(st, std::make_index_sequence<N>{});
    }

    template <size_t... Is>
    static void
    reclaim_all_impl(state_t &st, std::index_sequence<Is...>) {
        (reclaim_branch<Is>(st), ...);
    }
    static void
    reclaim_all(state_t &st) {
        reclaim_all_impl(st, std::make_index_sequence<N>{});
    }

    template <size_t I>
    static task<void>
    run_one(std::shared_ptr<state_t> state) {
        using task_type  = std::tuple_element_t<I, std::tuple<Tasks...>>;
        using value_type = typename task_type::value_type;
        try {
            std::any stored;
            if constexpr (std::is_void_v<value_type>) {
                co_await std::get<I>(state->tasks);
            } else {
                stored = std::any(co_await std::get<I>(state->tasks));
            }
            if (!state->done) {
                state->done   = true;
                state->result = when_any_result{I, std::is_void_v<value_type> ? std::any{} : std::move(stored), nullptr};
                reclaim_losers<I>(*state);
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        } catch (...) {
            if (!state->done) {
                state->done   = true;
                state->result = when_any_result{I, std::any{}, std::current_exception()};
                reclaim_losers<I>(*state);
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        }
    }

    template <size_t... Is>
    void
    start_all(std::index_sequence<Is...>) {
        // spawn_tracked records each run_one handle so losers can be reclaimed on win.
        // All branches are spawned (queued, not resumed) before any runs, so every slot
        // is populated before a winner can be decided.
        ((_state->branch_handles[Is] = coro_scheduler().spawn_tracked(run_one<Is>(_state))), ...);
    }

public:
    explicit when_any_awaiter(Tasks... tasks)
        : _state(std::make_shared<state_t>(std::move(tasks)...)) {}

    // Pending-teardown reclamation: if this awaiter is destroyed while still racing
    // (the awaiting coroutine's frame was unwound — e.g. it is itself a losing branch
    // of an OUTER when_any, or actor-scope cancellation) every branch is still parked,
    // so tear them all down. Once a winner has been decided every slot is already
    // cleared, so this is a no-op on the normal path.
    ~when_any_awaiter() {
        if (_state)
            reclaim_all(*_state);
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return N == 0;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        start_all(std::make_index_sequence<N>{});
    }

    result_type
    await_resume() {
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
auto
when_any(Tasks... tasks) {
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

    // See the variadic when_any_awaiter dtor: reclaim every still-parked branch if this
    // awaiter is destroyed while still racing. No-op once a winner cleared the slots.
    ~when_any_vector_awaiter() {
        if (_state)
            for (size_t j = 0; j < _state->branch_handles.size(); ++j)
                reclaim_branch(*_state, j);
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->tasks.empty();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation           = h;
        const size_t             n     = _state->tasks.size();
        std::shared_ptr<state_t> state = _state;
        // spawn_tracked records each run_one handle so the winner (and the dtor above)
        // can reclaim the losing branches. All are spawned before any runs.
        state->branch_handles.resize(n);
        for (size_t i = 0; i < n; ++i) {
            state->branch_handles[i] = coro_scheduler().spawn_tracked(run_one(state, i, n));
        }
    }

    result_type
    await_resume() {
        if (_state->exception)
            std::rethrow_exception(_state->exception);
        if (!_state->result)
            return result_type{0, std::any{}}; // empty vector edge case
        return std::move(*_state->result);
    }

private:
    struct state_t {
        std::vector<task<T>>       tasks;
        std::optional<result_type> result;
        std::exception_ptr         exception;
        bool                       done{false}; // single-thread
        std::coroutine_handle<>    continuation;
        // Spawned run_one frame per branch — see the variadic when_any_awaiter.
        std::vector<std::coroutine_handle<>> branch_handles;
        explicit state_t(std::vector<task<T>> t)
            : tasks(std::move(t)) {}
    };
    std::shared_ptr<state_t> _state;

    // Reclaim one losing branch: destroy its spawned run_one frame (cancel_spawned) and
    // the inner task<T> it awaited (forget() to scrub the scheduler queues, then the
    // task<T> dtor frees the frame and stops any watcher). Mirrors the variadic version.
    static void
    reclaim_branch(state_t &st, size_t j) {
        if (j >= st.branch_handles.size() || !st.branch_handles[j])
            return;
        auto &sched = coro_scheduler();
        sched.cancel_spawned(std::exchange(st.branch_handles[j], {}));
        if (auto inner = st.tasks[j].handle())
            sched.forget(inner);
        st.tasks[j] = task<T>{};
    }

    static task<void>
    run_one(std::shared_ptr<state_t> state, size_t i, size_t n) {
        (void) n;
        try {
            auto value = co_await state->tasks[i];
            if (!state->done) {
                state->done              = true;
                state->result            = result_type{i, std::any(std::move(value))};
                state->branch_handles[i] = {}; // winner self-reclaims at final_suspend
                for (size_t j = 0; j < state->branch_handles.size(); ++j)
                    reclaim_branch(*state, j);
                if (state->continuation)
                    schedule_via_current(state->continuation);
            }
        } catch (...) {
            if (!state->done) {
                state->done              = true;
                state->exception         = std::current_exception();
                state->result            = result_type{i, std::any{}};
                state->branch_handles[i] = {}; // winner self-reclaims at final_suspend
                for (size_t j = 0; j < state->branch_handles.size(); ++j)
                    reclaim_branch(*state, j);
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
auto
when_any(std::vector<task<T>> tasks) {
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
    timeout_error()
        : std::runtime_error("Operation timed out") {}
};

/**
 * @brief Awaiter that adds timeout to any operation (non-void version)
 * @tparam T Result type of the task
 *
 * The inner task is driven by a spawned `run_task` coroutine; the timeout is a single
 * self-stopping raw `ev_timer` (mirrors `qb::detail::ask_awaiter`) — NOT a spawned
 * `co_await sleep(timeout)` coroutine. A spawned timeout coroutine stays parked on its
 * sleep for the FULL timeout window even after the task wins, leaking its frame + the
 * ev_timer it is parked on until the original deadline (one zombie watcher per in-flight
 * call on a hot path). The raw timer is instead stopped the instant the awaiter resumes
 * (`finish()` from await_resume), so a call that completes before its timeout leaves no
 * lingering watcher behind.
 *
 * Uses shared_ptr<state_t> so the spawned `run_task` coroutine can still access the
 * shared state after the awaiter is destroyed when the outer coroutine resumes (the
 * inner task keeps running in the background until it finishes naturally — Finding
 * 2.B.4 — its result then dropped). The `ev_timer` lives in state_t (its `data` points
 * at the raw state_t, not the awaiter) and is always stopped via `finish()` before the
 * awaiter is destroyed, so the loop never holds a dangling timer pointer past the
 * awaiter's lifetime.
 */
template <typename T>
class timeout_awaiter {
    struct state_t {
        task<T>            inner_task;
        std::optional<T>   result;
        std::exception_ptr exception;
        // Plain bool: run_task and the ev_timer callback run cooperatively on the same
        // thread — only one can fire first, so the "first wins" check is sequential.
        bool                    completed{false};
        bool                    timed_out{false};
        std::coroutine_handle<> continuation;
        // Single self-stopping timeout watcher (see class doc). Lives here, not in the
        // awaiter, so it survives the awaiter's destruction while run_task is still
        // running; `finish()` always stops it before this state_t is destroyed.
        ev_timer timer{};
        bool     timer_started{false};

        explicit state_t(task<T> &&t)
            : inner_task(std::move(t)) {}

        // The timeout fired before the task completed: record it and resume the awaiting
        // coroutine. Guarded by `completed` so a task that already won (or a redundant
        // wake) is a no-op. Invoked from the ev_timer callback through the raw state
        // pointer in `timer.data`, never via the (possibly destroyed) awaiter.
        void
        resolve_timeout() {
            if (!completed) {
                completed = true;
                timed_out = true;
                if (continuation)
                    schedule_via_current(continuation);
            }
        }
    };

    std::shared_ptr<state_t> _state;
    qb::duration             _timeout;

    static task<void>
    run_task(std::shared_ptr<state_t> state) {
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

    // Stop the timeout watcher (idempotent). Called from await_resume (task or timeout
    // resolved the wait → stop now, not at the end of the timeout window) and from the
    // destructor (awaiter unwound mid-race). The ev_is_active guard covers the one-shot
    // already-fired case; the _state guard covers a moved-from awaiter.
    void
    finish() noexcept {
        if (_state && _state->timer_started) {
            if (ev_is_active(&_state->timer))
                ev_timer_stop(static_cast<struct ev_loop *>(listener::current.loop()), &_state->timer);
            _state->timer_started = false;
        }
    }

    static void
    on_timeout(struct ev_loop *, ev_timer *w, int) noexcept {
        if (auto *st = static_cast<state_t *>(w->data))
            st->resolve_timeout();
    }

public:
    timeout_awaiter(task<T> &&t, qb::duration timeout)
        : _state(std::make_shared<state_t>(std::move(t)))
        , _timeout(timeout) {}

    // Non-copyable / non-movable: `finish()` runs in the destructor and a stray copy or
    // moved-from husk would stop the shared timer out from under the live awaiter. The
    // helper functions return a prvalue (guaranteed copy elision), so neither is needed.
    timeout_awaiter(const timeout_awaiter &)            = delete;
    timeout_awaiter(timeout_awaiter &&)                 = delete;
    timeout_awaiter &operator=(const timeout_awaiter &) = delete;
    timeout_awaiter &operator=(timeout_awaiter &&)      = delete;

    ~timeout_awaiter() {
        finish();
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        coro_scheduler().spawn(run_task(_state));
        // Arm a single self-stopping ev_timer instead of spawning a `co_await sleep`
        // coroutine. `data` points at the raw state_t (kept alive by _state while the
        // timer can fire); ev_now_update refreshes libev's cached time so the timeout is
        // measured from now, not a stale loop iteration (mirrors timer_awaiter).
        auto loop = listener::current.loop();
        ev_timer_init(&_state->timer, &timeout_awaiter::on_timeout, qb::detail::to_ev_seconds(_timeout), 0.0);
        _state->timer.data = _state.get();
        ev_now_update(static_cast<struct ev_loop *>(loop));
        ev_timer_start(loop, &_state->timer);
        _state->timer_started = true;
    }

    T
    await_resume() {
        finish();
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
auto
coro_with_timeout(task<T> &&t, qb::duration timeout) {
    return timeout_awaiter<T>(std::move(t), timeout);
}

/**
 * @brief Specialization for void tasks
 *
 * Same raw self-stopping `ev_timer` design as the non-void `timeout_awaiter` — see its
 * class doc for the zombie-watcher rationale and the state_t/finish() lifetime contract.
 */
template <>
class timeout_awaiter<void> {
    struct state_t {
        task<void>              inner_task;
        std::exception_ptr      exception;
        bool                    completed{false}; // single-thread
        bool                    timed_out{false};
        std::coroutine_handle<> continuation;
        // Single self-stopping timeout watcher; stopped via finish() before this state_t
        // is destroyed. See the non-void timeout_awaiter for the full lifetime contract.
        ev_timer timer{};
        bool     timer_started{false};

        explicit state_t(task<void> &&t)
            : inner_task(std::move(t)) {}

        void
        resolve_timeout() {
            if (!completed) {
                completed = true;
                timed_out = true;
                if (continuation)
                    schedule_via_current(continuation);
            }
        }
    };

    std::shared_ptr<state_t> _state;
    qb::duration             _timeout;

    static task<void>
    run_task(std::shared_ptr<state_t> state) {
        try {
            co_await state->inner_task;
        } catch (...) {
            state->exception = std::current_exception();
        }
        if (!state->completed) {
            state->completed = true;
            if (state->continuation)
                schedule_via_current(state->continuation);
        }
    }

    void
    finish() noexcept {
        if (_state && _state->timer_started) {
            if (ev_is_active(&_state->timer))
                ev_timer_stop(static_cast<struct ev_loop *>(listener::current.loop()), &_state->timer);
            _state->timer_started = false;
        }
    }

    static void
    on_timeout(struct ev_loop *, ev_timer *w, int) noexcept {
        if (auto *st = static_cast<state_t *>(w->data))
            st->resolve_timeout();
    }

public:
    timeout_awaiter(task<void> &&t, qb::duration timeout)
        : _state(std::make_shared<state_t>(std::move(t)))
        , _timeout(timeout) {}

    // Non-copyable / non-movable: see the non-void timeout_awaiter.
    timeout_awaiter(const timeout_awaiter &)            = delete;
    timeout_awaiter(timeout_awaiter &&)                 = delete;
    timeout_awaiter &operator=(const timeout_awaiter &) = delete;
    timeout_awaiter &operator=(timeout_awaiter &&)      = delete;

    ~timeout_awaiter() {
        finish();
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->continuation = h;
        coro_scheduler().spawn(run_task(_state));
        auto loop = listener::current.loop();
        ev_timer_init(&_state->timer, &timeout_awaiter::on_timeout, qb::detail::to_ev_seconds(_timeout), 0.0);
        _state->timer.data = _state.get();
        ev_now_update(static_cast<struct ev_loop *>(loop));
        ev_timer_start(loop, &_state->timer);
        _state->timer_started = true;
    }

    void
    await_resume() {
        finish();
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
inline auto
coro_with_timeout(task<void> &&t, qb::duration timeout) {
    return timeout_awaiter<void>(std::move(t), timeout);
}

// =============================================================================
// Race - Wait for first, reclaim the losers
// =============================================================================

/**
 * @brief Race multiple tasks, return first result
 *
 * Semantic alias for `when_any` (first winner wins). As of the detached-frame
 * reclamation fix the losing branches are **torn down** the instant a winner is
 * decided: each loser's spawned `run_one` frame and the inner task it was parked
 * on are destroyed, stopping any libev watcher via the awaiter destructor (rather
 * than letting them run detached to completion — finding 2.B.3, superseded). The
 * winner's value is unchanged; only the discarded losers' wasted work + lingering
 * frames/watchers are eliminated. A loser is interrupted at its current suspension
 * point — do not rely on a losing branch running to completion for its side effects.
 *
 * @ingroup Coroutine
 */
template <typename... Tasks>
auto
race(Tasks... tasks) {
    return when_any(std::move(tasks)...);
}

/**
 * @brief Race vector of tasks
 *
 * See `race(Tasks...)`. Same loser-reclamation semantics (finding 2.B.3, superseded).
 * @ingroup Coroutine
 */
template <typename T>
auto
race(std::vector<task<T>> tasks) {
    return when_any(std::move(tasks));
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_COMBINATORS_H
