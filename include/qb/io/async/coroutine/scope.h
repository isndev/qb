/**
 * @file qb/io/async/coroutine/scope.h
 * @brief Coroutine scope for lifetime management
 *
 * Provides structured concurrency for coroutines with automatic cleanup.
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

#ifndef QB_IO_ASYNC_COROUTINE_SCOPE_H
#define QB_IO_ASYNC_COROUTINE_SCOPE_H

#include "task.h"
#include "utils.h"
#include "cancellation.h"
#include <vector>
#include <memory>
// NOTE: No <mutex> / <atomic> needed here — coroutine_scope and all its
// helpers run exclusively on one qb-io thread under the cooperative scheduler.
// Only one coroutine can be active at a time, so plain bools and plain
// vectors are sufficient.
#include <optional>
#include <tuple>
#include <utility>

#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
#include <cstdio>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif
#define QB_SCOPE_TRACE(fmt, ...) std::fprintf(stderr, "[scope] " fmt "\n", ##__VA_ARGS__)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
#define QB_SCOPE_TRACE(fmt, ...) ((void)0)
#endif

namespace qb::io::async {

/**
 * @brief Manages a collection of coroutines with structured concurrency
 *
 * A coroutine_scope owns spawned coroutines and ensures they complete
 * before the scope is destroyed. It supports cancellation and cleanup.
 *
 * Single-thread: all methods and spawned tasks run on the same thread
 * as the scheduler; do not share a scope across threads.
 *
 * Usage:
 * @code
 * task<void> parent() {
 *     coroutine_scope scope;
 *
 *     scope.spawn(worker1());
 *     scope.spawn(worker2());
 *     scope.spawn(worker3());
 *
 *     // Wait for all
 *     co_await scope.join_all();
 * }
 * @endcode
 */
class coroutine_scope {
public:
    /**
     * @brief Policy for scope cleanup
     */
    enum class cleanup_policy {
        join_all,       // Wait for all on destruction
        cancel_all,     // Cancel all on destruction
        detach          // Let coroutines run independently
    };

private:
    struct task_state {
        bool completed{false};  // plain bool: single-thread cooperative
        std::exception_ptr error;
    };

    std::vector<std::shared_ptr<task_state>> _tasks;
    cancellation_token _cancel_token;
    cleanup_policy _policy;

public:
    /**
     * @brief Create a scope with given cleanup policy
     * @param policy How to handle tasks on destruction
     */
    explicit coroutine_scope(cleanup_policy policy = cleanup_policy::cancel_all)
        : _policy(policy) {}

    /**
     * @brief Destructor - applies cleanup policy
     */
    ~coroutine_scope() {
        QB_SCOPE_TRACE("dtor policy=%d tasks=%zu", (int)_policy, _tasks.size());
        switch (_policy) {
            case cleanup_policy::join_all:
                // Best-effort: do not block destructor. Caller should co_await join_all()
                // before scope is destroyed, or run the event loop after (e.g. run_for) to
                // let spawned tasks complete.
                break;

            case cleanup_policy::cancel_all:
                cancel_all();
                break;

            case cleanup_policy::detach:
                // Just clear - let them run
                _tasks.clear();
                break;
        }
    }

    // Non-copyable, movable
    coroutine_scope(const coroutine_scope&) = delete;
    coroutine_scope& operator=(const coroutine_scope&) = delete;

    coroutine_scope(coroutine_scope&& other) noexcept
        : _tasks(std::move(other._tasks))
        , _cancel_token(std::move(other._cancel_token))
        , _policy(other._policy) {}

    coroutine_scope& operator=(coroutine_scope&& other) noexcept {
        if (this != &other) {
            _tasks = std::move(other._tasks);
            _cancel_token = std::move(other._cancel_token);
            _policy = other._policy;
        }
        return *this;
    }

    /**
     * @brief Spawn a task in this scope
     * @tparam T Task return type
     * @param t Task to spawn
     *
     * Wrapper is a function (not a lambda) so state and inner are in the coroutine
     * frame; capturing them in a lambda would expose the same lifetime bug as when_all.
     */
    template <typename T>
    void spawn(task<T>&& t) {
        auto state = std::make_shared<task_state>();
        _tasks.push_back(state);
        task<void> wrapped_task = run_wrapped(std::move(state), std::move(t));
        QB_SCOPE_TRACE("spawn total=%zu", _tasks.size());
        coro_scheduler().spawn(std::move(wrapped_task));
    }

private:
    template <typename T>
    static task<void> run_wrapped(std::shared_ptr<task_state> state, task<T> inner) {
        try {
            co_await inner;
        } catch (...) {
            state->error = std::current_exception();
        }
        state->completed = true;
        QB_SCOPE_TRACE("wrapper done state=%p", (void*)state.get());
    }

public:

    /**
     * @brief Spawn a cancellable task
     * @tparam T Task return type
     * @param t Task to spawn
     * @param token Cancellation token (uses scope's token if not provided)
     */
    template <typename T>
    void spawn_cancellable(task<T>&& t, std::optional<cancellation_token> token = std::nullopt) {
        cancellation_token use_token = token.value_or(_cancel_token);
        // Use a static function (not a lambda) to avoid the dangling-lambda-pointer
        // bug: if a local lambda were used here, the compiler might store a pointer
        // to it in the spawned coroutine frame; the lambda would dangle after
        // spawn_cancellable returns.
        spawn(cancellable_wrapper<T>(std::move(t), std::move(use_token)));
    }

private:
    // Static free function: inner and token are VALUE parameters in the coroutine
    // frame, so their lifetime is independent of the call site.
    template <typename T>
    static task<T> cancellable_wrapper(task<T> inner, cancellation_token token) {
        co_return co_await make_cancellable(std::move(inner), std::move(token), true);
    }

public:

    /**
     * @brief Cancel all tasks in this scope
     */
    void cancel_all() {
        _cancel_token.cancel();
    }

    /**
     * @brief Get the scope's cancellation token
     */
    cancellation_token cancel_token() const {
        return _cancel_token;
    }

    /**
     * @brief Wait for all tasks to complete
     * @return Task that completes when all tasks done
     */
    task<void> join_all() {
        QB_SCOPE_TRACE("join_all start tasks=%zu", _tasks.size());
        while (true) {
            bool all_done = true;
            for (const auto& state : _tasks) {
                if (!state->completed) {
                    all_done = false;
                    break;
                }
            }

            if (all_done) {
                QB_SCOPE_TRACE("join_all done");
                for (const auto& state : _tasks) {
                    if (state->error)
                        std::rethrow_exception(state->error);
                }
                co_return;
            }

            co_await sleep(std::chrono::milliseconds(1));
        }
    }

    /**
     * @brief Wait for any task to complete
     * @return Index of completed task
     */
    task<size_t> join_any() {
        QB_SCOPE_TRACE("join_any start tasks=%zu", _tasks.size());
        while (true) {
            for (size_t i = 0; i < _tasks.size(); ++i) {
                if (_tasks[i]->completed) {
                    QB_SCOPE_TRACE("join_any found index=%zu", i);
                    co_return i;
                }
            }
            co_await sleep(std::chrono::milliseconds(1));
        }
    }

    /**
     * @brief Wait for all with timeout
     * @param timeout Maximum time to wait
     * @return true if all completed, false if timeout
     */
    task<bool> join_all_for(std::chrono::milliseconds timeout) {
        auto start = std::chrono::steady_clock::now();

        while (true) {
            {
                bool all_done = true;
                for (const auto& state : _tasks) {
                    if (!state->completed) {
                        all_done = false;
                        break;
                    }
                }

                if (all_done) {
                    co_return true;
                }
            }

            if (std::chrono::steady_clock::now() - start > timeout) {
                co_return false;
            }

            co_await sleep(std::chrono::milliseconds(1));
        }
    }

    /**
     * @brief Get number of active (non-completed) tasks
     */
    size_t active_count() const {
        size_t count = 0;
        for (const auto& state : _tasks) {
            if (!state->completed)
                ++count;
        }
        return count;
    }

    size_t total_count() const noexcept { return _tasks.size(); }

    /**
     * @brief Remove completed task entries from the internal list
     */
    void prune_completed() {
        _tasks.erase(
            std::remove_if(_tasks.begin(), _tasks.end(),
                [](const auto& state) { return state->completed; }),
            _tasks.end()
        );
    }

    bool empty() const { return active_count() == 0; }

    /**
     * @brief Rethrow first error encountered (if any)
     */
    void rethrow_if_error() const {
        for (const auto& state : _tasks) {
            if (state->error)
                std::rethrow_exception(state->error);
        }
    }
};

/**
 * @brief Scope guard that automatically joins on destruction
 *
 * Usage:
 * @code
 * task<void> parent() {
 *     joining_scope scope;
 *     scope.spawn(worker());
 *     // Automatically joins when scope goes out of scope
 * }
 * @endcode
 */
class joining_scope : public coroutine_scope {
public:
    joining_scope() : coroutine_scope(cleanup_policy::join_all) {}
};

/**
 * @brief Scope guard that cancels on destruction (default behavior)
 */
class cancelling_scope : public coroutine_scope {
public:
    cancelling_scope() : coroutine_scope(cleanup_policy::cancel_all) {}
};

/**
 * @brief Scope that detaches tasks on destruction
 */
class detaching_scope : public coroutine_scope {
public:
    detaching_scope() : coroutine_scope(cleanup_policy::detach) {}
};

/**
 * @brief Helper: run a task and store its result in an optional
 * @tparam T Task return type
 * @param t Task to run
 * @param result Where to store the result
 * @ingroup Coroutine
 */
template <typename T>
task<void> capture_result(task<T> t, std::optional<T>& result) {
    result = co_await std::move(t);
}

namespace detail {
template <typename Scope, typename ResultsTuple, typename TasksTuple, size_t... Is>
void spawn_capture_impl(Scope& scope, ResultsTuple& results, TasksTuple& tasks_tuple,
                        std::index_sequence<Is...>) {
    (scope.spawn(capture_result(std::move(std::get<Is>(tasks_tuple)), std::get<Is>(results))), ...);
}
} // namespace detail

/**
 * @brief Execute multiple tasks concurrently and wait for all
 * @tparam Tasks Task types
 * @param tasks Tasks to execute
 * @return Task that returns tuple of results
 * @ingroup Coroutine
 */
template <typename... Tasks>
task<std::tuple<typename Tasks::value_type...>> parallel(Tasks... tasks) {
    coroutine_scope scope(coroutine_scope::cleanup_policy::join_all);

    using result_tuple = std::tuple<std::optional<typename Tasks::value_type>...>;
    result_tuple results;
    std::tuple<Tasks...> tasks_tuple(std::move(tasks)...);

    detail::spawn_capture_impl(scope, results, tasks_tuple, std::index_sequence_for<Tasks...>{});

    co_await scope.join_all();

    co_return std::apply([](auto&&... opts) {
        return std::tuple(*opts...);
    }, results);
}

/**
 * @brief Run function with scoped task lifetime
 * @tparam F Function type
 * @param f Function taking coroutine_scope&
 * @return Task with function result
 * @ingroup Coroutine
 */
template <typename F>
task<std::invoke_result_t<F>> with_scope(F f) {
    coroutine_scope scope;
    co_return co_await f(scope);
}

/**
 * @brief Repeat a task until cancelled or predicate fails
 * @tparam F Task factory
 * @tparam P Predicate type
 * @param factory Function creating new tasks
 * @param should_continue Predicate checking if should continue
 * @param cancel_token Cancellation token
 * @return Task that loops
 * @ingroup Coroutine
 */
template <typename F, typename P>
task<void> repeat_while(F factory, P should_continue, cancellation_token cancel_token = {}) {
    while (!cancel_token.is_cancelled() && should_continue()) {
        auto t = factory();
        co_await make_cancellable(std::move(t), cancel_token, false);
    }
}

namespace detail {

// Free function (not a lambda): parameters are stored BY VALUE in the coroutine
// frame by the standard, so there is no dangling-lambda-pointer risk here even
// when this task is spawned from inside a loop.
template <typename F, typename Item, typename R>
task<void> parallel_map_worker(semaphore* sem, F fn, std::optional<R>* result, Item item) {
    auto guard = co_await sem->scoped_acquire();
    *result    = co_await fn(item);
}

} // namespace detail

/**
 * @brief Parallel map - apply function to all items concurrently
 * @tparam Range Range type
 * @tparam F Function type
 * @param items Items to process
 * @param f Function to apply
 * @param max_concurrency Maximum parallel tasks
 * @return Task with vector of results
 * @ingroup Coroutine
 */
template <typename Range, typename F>
auto parallel_map(const Range& items, F f, size_t max_concurrency = 10)
    -> task<std::vector<typename std::invoke_result_t<F, typename Range::value_type>::value_type>>
{
    using result_type      = std::invoke_result_t<F, typename Range::value_type>;
    using inner_result_type = typename result_type::value_type;

    semaphore sem(max_concurrency);
    coroutine_scope scope;

    std::vector<std::optional<inner_result_type>> results(items.size());

    size_t i = 0;
    for (const auto& item : items) {
        // Pass sem, f and result slot as raw pointers / by-value copies into a
        // free function. parallel_map suspends at join_all() below, so sem and
        // results[i] remain valid for the entire lifetime of the workers.
        scope.spawn(detail::parallel_map_worker(&sem, f, &results[i], item));
        ++i;
    }

    co_await scope.join_all();

    std::vector<inner_result_type> final_results;
    final_results.reserve(results.size());
    for (auto& opt : results) {
        if (opt) {
            final_results.push_back(std::move(*opt));
        }
    }

    co_return final_results;
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SCOPE_H
