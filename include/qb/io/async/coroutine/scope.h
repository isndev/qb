/**
 * @file qb/io/async/coroutine/scope.h
 * @brief Coroutine scope for lifetime management
 *
 * Provides structured concurrency for coroutines with automatic cleanup.
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

#ifndef QB_IO_ASYNC_COROUTINE_SCOPE_H
#define QB_IO_ASYNC_COROUTINE_SCOPE_H

#include "task.h"
#include "utils.h"
#include "cancellation.h"
#include <qb/system/timestamp.h> // qb::duration
#include <algorithm>
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
// Standard C++20 __VA_OPT__ elides the comma when no trailing args are passed
// (MSVC needs the conformant preprocessor /Zc:preprocessor, enabled by qb's build).
#define QB_SCOPE_TRACE(fmt, ...) std::fprintf(stderr, "[scope] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define QB_SCOPE_TRACE(fmt, ...) ((void) 0)
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
        join_all,   // Wait for all on destruction (best-effort)
        cancel_all, // Cancel all on destruction (default)
        detach      // Let coroutines run independently
    };

private:
    struct task_state {
        bool               completed{false};
        std::exception_ptr error;
    };

    /**
     * @brief Stable heap state shared between the scope and spawned coroutines.
     *
     * join_all_waiter_slots / join_any_waiter_slots hold shared_ptr to handle
     * slots. An awaiter's await_resume() nulls its slot (*slot = {}) before the
     * coroutine frame is allowed to be destroyed. on_task_done() reads *slot and
     * skips null/done entries — no dangling-handle dereference possible.
     */
    struct scope_impl {
        using handle_slot = std::shared_ptr<std::coroutine_handle<>>;

        std::vector<std::shared_ptr<task_state>> tasks;
        size_t                                   active_count{0};
        std::vector<handle_slot>                 join_all_waiter_slots; ///< one per concurrent join_all
        std::vector<handle_slot>                 join_any_waiter_slots; ///< one per concurrent join_any
        std::exception_ptr                       first_error;

        static void
        wake_slots(std::vector<handle_slot> &slots) {
            for (auto &slot : slots) {
                if (!slot)
                    continue;
                auto h = *slot;
                if (h && !h.done())
                    schedule_via_current(h);
            }
            slots.clear();
        }

        void
        on_task_done(std::exception_ptr err) {
            QB_SCOPE_TRACE("on_task_done enter active_before=%zu err=%d", active_count, (int) (!!err));
            if (err && !first_error)
                first_error = err;
            --active_count;
            QB_SCOPE_TRACE("on_task_done active_after=%zu any_slots=%zu all_slots=%zu", active_count, join_any_waiter_slots.size(),
                           join_all_waiter_slots.size());
            // join_any: wake all waiters; they re-check which task completed
            if (!join_any_waiter_slots.empty()) {
                QB_SCOPE_TRACE("on_task_done waking %zu join_any slots", join_any_waiter_slots.size());
                wake_slots(join_any_waiter_slots);
            }
            // join_all: wake only when the very last task finishes
            if (active_count == 0 && !join_all_waiter_slots.empty()) {
                QB_SCOPE_TRACE("on_task_done waking %zu join_all slots", join_all_waiter_slots.size());
                wake_slots(join_all_waiter_slots);
            }
        }
    };

    std::shared_ptr<scope_impl> _impl;
    cancellation_token          _cancel_token;
    cleanup_policy              _policy;

public:
    explicit coroutine_scope(cleanup_policy policy = cleanup_policy::cancel_all)
        : _impl(std::make_shared<scope_impl>())
        , _policy(policy) {}

    /**
     * @brief Destructor - applies cleanup policy
     *
     * Finding 2.B.12/2.B.13:
     *   - `join_all` is *best-effort*: if the caller forgot to `co_await
     *     join_all()` before the scope goes out of scope, spawned tasks
     *     keep running via `scope_impl` (they hold a shared_ptr to it).
     *     We now emit a debug-only warning so the misuse is discoverable.
     *   - `detach` clears our list; tasks keep running because
     *     `scope_impl` is shared.
     *   - `cancel_all` signals the scope's token; coroutines that honour
     *     cancellation terminate cooperatively.
     */
    ~coroutine_scope() {
        QB_SCOPE_TRACE("dtor policy=%d tasks=%zu", (int) _policy, _impl ? _impl->tasks.size() : 0u);
        if (!_impl)
            return;
        switch (_policy) {
            case cleanup_policy::join_all:
#ifndef NDEBUG
                if (_impl->active_count != 0) {
                    std::fprintf(stderr,
                                 "[scope][warn] ~coroutine_scope(join_all) destroyed with "
                                 "%zu active tasks — you should `co_await join_all()` before "
                                 "the scope goes out of scope.\n",
                                 _impl->active_count);
                }
#endif
                break;

            case cleanup_policy::cancel_all:
                cancel_all();
                break;

            case cleanup_policy::detach:
                _impl->tasks.clear();
                break;
        }
    }

    // Non-copyable, movable (_impl is a shared_ptr so move is always safe)
    coroutine_scope(const coroutine_scope &)            = delete;
    coroutine_scope &operator=(const coroutine_scope &) = delete;

    coroutine_scope(coroutine_scope &&other) noexcept
        : _impl(std::move(other._impl))
        , _cancel_token(std::move(other._cancel_token))
        , _policy(other._policy) {}

    coroutine_scope &
    operator=(coroutine_scope &&other) noexcept {
        if (this != &other) {
            _impl         = std::move(other._impl);
            _cancel_token = std::move(other._cancel_token);
            _policy       = other._policy;
        }
        return *this;
    }

    /**
     * @brief Spawn a task in this scope.
     *
     * Ownership note: the inner task's coroutine frame is managed by
     * run_wrapped, which holds a shared_ptr to the scope's impl.
     */
    template <typename T>
    void
    spawn(task<T> t) {
        auto state = std::make_shared<task_state>();
        _impl->tasks.push_back(state);
        ++_impl->active_count;
        QB_SCOPE_TRACE("spawn impl=%p active=%zu", (void *) _impl.get(), _impl->active_count);
        coro_scheduler().spawn(run_wrapped(_impl, std::move(state), std::move(t)));
    }

    /**
     * @brief Spawn a callable (lambda/functor) safely — closure is owned.
     *
     * Identical motivation as CoroutineScheduler::spawn(Callable): the
     * callable is moved into an owning wrapper coroutine frame so its
     * captures remain valid for the full coroutine lifetime.
     *
     * Usage:
     * @code
     * // BROKEN — lambda can dangle at end of loop iteration:
     * scope.spawn([data, i]() -> task<void> { co_await process(data, i); }());
     *
     * // SAFE — pass the lambda without ():
     * scope.spawn([data, i]() -> task<void> { co_await process(data, i); });
     * @endcode
     */
    template <typename Callable>
    requires std::invocable<Callable> && (!std::same_as<std::decay_t<Callable>, task<void>>)
    void
    spawn(Callable fn) {
        spawn(owned_invoke_(std::move(fn)));
    }

private:
    /**
     * @brief Wrapper coroutine that owns a callable inside its frame.
     *
     * F is a VALUE PARAMETER — the standard copies/moves it into the
     * coroutine state, so the original closure lifetime does not matter.
     */
    template <typename F>
    static task<void>
    owned_invoke_(F fn) {
        co_await fn();
    }

    template <typename T>
    static task<void>
    run_wrapped(std::shared_ptr<scope_impl> impl, std::shared_ptr<task_state> state, task<T> inner) {
        QB_SCOPE_TRACE("run_wrapped entry impl=%p active=%zu", (void *) impl.get(), impl->active_count);
        std::exception_ptr err;
        try {
            co_await inner;
        } catch (...) {
            err = std::current_exception();
            QB_SCOPE_TRACE("run_wrapped caught exception");
        }
        state->completed = true;
        state->error     = err;
        QB_SCOPE_TRACE("run_wrapped pre-done impl=%p active_before=%zu", (void *) impl.get(), impl->active_count);
        impl->on_task_done(err);
        QB_SCOPE_TRACE("run_wrapped post-done impl=%p", (void *) impl.get());
    }

public:
    /**
     * @brief Spawn a cancellable task
     * @tparam T Task return type
     * @param t Task to spawn
     * @param token Cancellation token (uses scope's token if not provided)
     */
    template <typename T>
    void
    spawn_cancellable(task<T> &&t, std::optional<cancellation_token> token = std::nullopt) {
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
    static task<T>
    cancellable_wrapper(task<T> inner, cancellation_token token) {
        co_return co_await make_cancellable(std::move(inner), std::move(token), true);
    }

public:
    /**
     * @brief Cancel all tasks in this scope
     */
    void
    cancel_all() {
        _cancel_token.cancel();
    }

    /**
     * @brief Get the scope's cancellation token
     */
    cancellation_token
    cancel_token() const {
        return _cancel_token;
    }

    /**
     * @brief Wait for all tasks to complete (event-driven, zero polling)
     *
     * Suspends the caller directly; the last completing task wakes it.
     * Rethrows the first error encountered, if any.
     */
    task<void>
    join_all() {
        QB_SCOPE_TRACE("join_all start active=%zu", _impl->active_count);

        // Slot is shared with scope_impl. await_resume() nulls *slot so
        // on_task_done() can never use a dangling handle.
        auto slot = std::make_shared<std::coroutine_handle<>>();

        struct join_all_awaiter {
            std::shared_ptr<scope_impl>              impl;
            std::shared_ptr<std::coroutine_handle<>> slot;

            [[nodiscard]] bool
            await_ready() const noexcept {
                return impl->active_count == 0;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                if (impl->active_count == 0) {
                    schedule_via_current(h);
                    return;
                }
                *slot = h;
                impl->join_all_waiter_slots.push_back(slot);
            }

            void
            await_resume() noexcept {
                *slot = {};
            }
        };

        co_await join_all_awaiter{_impl, slot};
        QB_SCOPE_TRACE("join_all done");
        if (_impl->first_error)
            std::rethrow_exception(_impl->first_error);
    }

    /**
     * @brief Wait for any task to complete (event-driven)
     * @return Index of the first completed task, or total_count() if empty
     */
    task<size_t>
    join_any() {
        QB_SCOPE_TRACE("join_any start tasks=%zu", _impl->tasks.size());

        auto slot = std::make_shared<std::coroutine_handle<>>();

        struct join_any_awaiter {
            std::shared_ptr<scope_impl>              impl;
            std::shared_ptr<std::coroutine_handle<>> slot;

            [[nodiscard]] bool
            await_ready() const noexcept {
                for (const auto &t : impl->tasks)
                    if (t->completed)
                        return true;
                return false;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                for (const auto &t : impl->tasks) {
                    if (t->completed) {
                        schedule_via_current(h);
                        return;
                    }
                }
                *slot = h;
                impl->join_any_waiter_slots.push_back(slot);
            }

            size_t
            await_resume() noexcept {
                *slot = {};
                for (size_t i = 0; i < impl->tasks.size(); ++i)
                    if (impl->tasks[i]->completed)
                        return i;
                return impl->tasks.size();
            }
        };

        co_return co_await join_any_awaiter{_impl, slot};
    }

    /**
     * @brief Wait for all tasks with a wall-clock deadline (event-driven)
     *
     * Safety: the timer coroutine holds a shared_ptr to the handle slot.
     * await_resume() nulls the slot before the join_all_for frame can be
     * destroyed, so the timer never calls schedule_via_current on a dangling
     * handle even if it fires after the awaiting coroutine has moved on.
     *
     * @return true if all completed within timeout, false on timeout
     */
    task<bool>
    join_all_for(qb::duration timeout) {
        if (_impl->active_count == 0)
            co_return true;

        // Single shared slot used by BOTH on_task_done and the timer.
        // await_resume() nulls it — whichever path wins, the other finds null.
        auto slot = std::make_shared<std::coroutine_handle<>>();

        struct timed_join_awaiter {
            std::shared_ptr<scope_impl>              impl;
            std::shared_ptr<std::coroutine_handle<>> slot;
            qb::duration                             timeout_ms;

            [[nodiscard]] bool
            await_ready() const noexcept {
                return impl->active_count == 0;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                if (impl->active_count == 0) {
                    schedule_via_current(h);
                    return;
                }
                *slot = h;
                impl->join_all_waiter_slots.push_back(slot);
                // Timer also holds the slot; reads *slot on expiry.
                coro_scheduler().spawn(join_all_timer(slot, timeout_ms));
            }

            bool
            await_resume() noexcept {
                *slot = {}; // invalidate — timer and on_task_done find null
                return impl->active_count == 0;
            }
        };

        co_return co_await timed_join_awaiter{_impl, slot, timeout};
    }

    // -----------------------------------------------------------------------
    // Inspection helpers (public)
    // -----------------------------------------------------------------------

    /** @brief Number of tasks not yet completed */
    size_t
    active_count() const noexcept {
        return _impl->active_count;
    }

    size_t
    total_count() const noexcept {
        return _impl->tasks.size();
    }

    /** @brief Remove completed entries from the internal list to free memory */
    void
    prune_completed() {
        auto &tasks = _impl->tasks;
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const auto &s) { return s->completed; }), tasks.end());
    }

    bool
    empty() const noexcept {
        return _impl->active_count == 0;
    }

    /** @brief Rethrow first stored error, if any */
    void
    rethrow_if_error() const {
        if (_impl->first_error)
            std::rethrow_exception(_impl->first_error);
    }

private:
    /**
     * @brief Timer coroutine for join_all_for.
     *
     * Reads the slot after the delay. If await_resume() already nulled it,
     * the join completed normally — no-op. If not, the timeout wins: wake the
     * join_all_for awaiter.
     *
     * Parameters are stored by VALUE in the coroutine frame — no dangling refs.
     */
    static task<void>
    join_all_timer(std::shared_ptr<std::coroutine_handle<>> slot, qb::duration delay) {
        co_await sleep(delay);
        QB_SCOPE_TRACE("join_all_timer fired");
        auto h = *slot;
        if (h && !h.done())
            schedule_via_current(h);
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
    joining_scope()
        : coroutine_scope(cleanup_policy::join_all) {}
};

/**
 * @brief Scope guard that cancels on destruction (default behavior)
 */
class cancelling_scope : public coroutine_scope {
public:
    cancelling_scope()
        : coroutine_scope(cleanup_policy::cancel_all) {}
};

/**
 * @brief Scope that detaches tasks on destruction
 */
class detaching_scope : public coroutine_scope {
public:
    detaching_scope()
        : coroutine_scope(cleanup_policy::detach) {}
};

/**
 * @brief Helper: run a task and store its result in an optional
 * @tparam T Task return type
 * @param t Task to run
 * @param result Where to store the result
 * @ingroup Coroutine
 */
template <typename T>
task<void>
capture_result(task<T> t, std::optional<T> &result) {
    result = co_await std::move(t);
}

namespace detail {
template <typename Scope, typename ResultsTuple, typename TasksTuple, size_t... Is>
void
spawn_capture_impl(Scope &scope, ResultsTuple &results, TasksTuple &tasks_tuple, std::index_sequence<Is...>) {
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
task<std::tuple<typename Tasks::value_type...>>
parallel(Tasks... tasks) {
    coroutine_scope scope(coroutine_scope::cleanup_policy::join_all);

    using result_tuple = std::tuple<std::optional<typename Tasks::value_type>...>;
    result_tuple         results;
    std::tuple<Tasks...> tasks_tuple(std::move(tasks)...);

    detail::spawn_capture_impl(scope, results, tasks_tuple, std::index_sequence_for<Tasks...>{});

    co_await scope.join_all();

    co_return std::apply([](auto &&...opts) { return std::tuple(*opts...); }, results);
}

/**
 * @brief Run function with scoped task lifetime
 * @tparam F Function type
 * @param f Function taking coroutine_scope&
 * @return Task with function result
 * @ingroup Coroutine
 */
template <typename F>
auto
with_scope(F f) -> task<typename std::invoke_result_t<F, coroutine_scope &>::value_type> {
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
task<void>
repeat_while(F factory, P should_continue, cancellation_token cancel_token = {}) {
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
task<void>
parallel_map_worker(semaphore *sem, F fn, std::optional<R> *result, Item item) {
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
auto
parallel_map(const Range &items, F f, size_t max_concurrency = 10)
    -> task<std::vector<typename std::invoke_result_t<F, typename Range::value_type>::value_type>> {
    using result_type       = std::invoke_result_t<F, typename Range::value_type>;
    using inner_result_type = typename result_type::value_type;

    semaphore       sem(max_concurrency);
    coroutine_scope scope;

    std::vector<std::optional<inner_result_type>> results(items.size());

    size_t i = 0;
    for (const auto &item : items) {
        // Pass sem, f and result slot as raw pointers / by-value copies into a
        // free function. parallel_map suspends at join_all() below, so sem and
        // results[i] remain valid for the entire lifetime of the workers.
        scope.spawn(detail::parallel_map_worker(&sem, f, &results[i], item));
        ++i;
    }

    co_await scope.join_all();

    std::vector<inner_result_type> final_results;
    final_results.reserve(results.size());
    for (auto &opt : results) {
        if (opt) {
            final_results.push_back(std::move(*opt));
        }
    }

    co_return final_results;
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_SCOPE_H
