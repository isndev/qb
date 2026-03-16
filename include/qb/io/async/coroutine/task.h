/**
 * @file qb/io/async/coroutine/task.h
 * @brief C++23 coroutine task type for QB async I/O
 *
 * This file defines the task<T> class template, the primary coroutine type
 * for the QB framework. It integrates with libev for suspend/resume operations
 * and provides zero-cost abstraction in optimized builds.
 *
 * CRITICAL IMPLEMENTATION NOTES:
 * ==============================
 *
 * 1. VARIANT INITIALIZATION (CRITICAL BUG PREVENTION):
 *    The promise_type's result_ variant MUST be explicitly initialized to
 *    std::monostate (index 0). Without explicit initialization, the variant
 *    may contain a default-constructed T instead, causing await_ready() to
 *    return true prematurely and await_resume() to return uninitialized values.
 *    
 *    REQUIRED: promise_type() : result_(std::in_place_index<0>) {}
 *
 * 2. LAMBDA COROUTINE CAPTURES (DANGLING REFERENCE PREVENTION):
 *    When creating coroutines from lambdas, temporary lambda objects create
 *    dangling references after the first suspension point.
 *
 *    UNSAFE - Temporary lambda:
 *    @code
 *    auto t = [&data]() -> task<void> {
 *        co_await sleep(100ms);
 *        use(data);  // DANGLING! Lambda destroyed before resume
 *    }();
 *    @endcode
 *
 *    SAFE - Store lambda in variable:
 *    @code
 *    auto coro_fn = [&data]() -> task<void> {
 *        co_await sleep(100ms);
 *        use(data);
 *    };
 *    auto t = coro_fn();  // Lambda stays alive
 *    @endcode
 *
 *    BEST - Use regular functions or capture by pointer:
 *    @code
 *    task<void> process_data(Data* data) {
 *        co_await sleep(100ms);
 *        use(*data);
 *    }
 *    @endcode
 *
 * 3. SYMMETRIC TRANSFER:
 *    This implementation uses symmetric transfer (returning coroutine_handle
 *    from await_suspend and final_suspend) to prevent stack overflow in deep
 *    coroutine chains. The compiler handles the transfer without recursion.
 *
 * 4. MOVE SEMANTICS:
 *    task<T> is move-only. The handle is transferred on move, and the moved-from
 *    task becomes empty. Always use std::move when passing tasks to spawn() or
 *    other functions.
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

#ifndef QB_IO_ASYNC_COROUTINE_TASK_H
#define QB_IO_ASYNC_COROUTINE_TASK_H

#include <coroutine>
#include <exception>
#include <variant>
#include <utility>
#include <atomic>
#include <type_traits>

// Debug trace macro (defined early for use in promise_type)
#ifdef QB_DEBUG_COROUTINES
#include <iostream>
#define QB_CORO_TRACE(id, stage) \
    std::cerr << "[CORO] " << id << ": " << stage << "\n"
#else
#define QB_CORO_TRACE(id, stage) ((void)0)
#endif

namespace qb::io::async {

// Forward declaration
class CoroutineScheduler;

// schedule_via_current is defined in scheduler.h - include after this file
// or use the externally defined version

/**
 * @brief Coroutine task type for QB async operations
 *
 * task<T> is the primary return type for coroutines in the QB framework.
 * It integrates with the libev event loop for suspend/resume operations.
 *
 * USAGE GUIDELINES:
 * ================
 *
 * Basic Usage:
 * @code
 * qb::io::async::task<int> fetch_data() {
 *     co_await qb::io::async::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * }
 * @endcode
 *
 * Spawning Tasks:
 * @code
 * auto t = fetch_data();
 * qb::io::async::coro_scheduler().spawn(std::move(t));  // Note: std::move required
 * @endcode
 *
 * Awaiting Tasks:
 * @code
 * task<void> caller() {
 *     int result = co_await fetch_data();  // Suspends until fetch_data completes
 *     std::cout << "Got: " << result << "\n";
 * }
 * @endcode
 *
 * CRITICAL: Lambda Coroutine Safety
 * ==================================
 * When creating coroutines from lambdas, avoid temporary lambda objects:
 *
 * WRONG - Dangling reference:
 * @code
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back([&i]() -> task<int> {  // WRONG: captures &i
 *         co_await sleep(10ms);
 *         co_return i * 10;  // UNDEFINED: i may be out of scope
 *     }());
 * }
 * @endcode
 *
 * CORRECT - Pass by value:
 * @code
 * auto worker = [](int id) -> task<int> {  // id passed by value
 *     co_await sleep(10ms);
 *     co_return id * 10;
 * };
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back(worker(i));  // Safe: i copied into parameter
 * }
 * @endcode
 *
 * @tparam T The return type of the coroutine (void for no return value)
 * @ingroup Coroutine
 */
template <typename T = void>
class task {
public:
    using value_type = T;

    /**
     * @brief Promise type for task<T> coroutines
     *
     * Implements the coroutine protocol for integration with libev.
     */
    struct promise_type {
        /**
         * @brief Storage for the coroutine result
         *
         * CRITICAL: This variant MUST be explicitly initialized to index 0
         * (std::monostate) in the constructor. Without explicit initialization,
         * the variant may contain a default-constructed T, causing premature
         * ready state and undefined behavior.
         *
         * Index 0: no result yet (monostate) - INITIAL STATE
         * Index 1: success (T) - set by return_value()
         * Index 2: exception (exception_ptr) - set by unhandled_exception()
         */
        std::variant<std::monostate, T, std::exception_ptr> result_;

        /**
         * @brief Continuation handle for coroutine composition
         *
         * When this coroutine completes, this handle is resumed.
         */
        std::coroutine_handle<> continuation_;

        /**
         * @brief Pointer to the scheduler managing this coroutine
         */
        CoroutineScheduler* scheduler_ = nullptr;

        /**
         * @brief Coroutine ID for debugging/tracing
         */
#ifdef QB_DEBUG_COROUTINES
        static inline std::atomic<std::size_t> next_id{0};
        std::size_t coro_id_ = next_id.fetch_add(1, std::memory_order_relaxed);
#endif

        /**
         * @brief Constructor - CRITICAL: Explicitly initializes result_ to monostate
         *
         * This explicit initialization is REQUIRED to prevent undefined behavior.
         * Without it, the variant may contain a default-constructed T instead of
         * std::monostate, causing is_ready() to return true prematurely.
         */
        promise_type() : result_(std::in_place_index<0>) {}

        /**
         * @brief Construct the task return object
         * @return task bound to this promise
         */
        task get_return_object() {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * @brief Initial suspension point
         *
         * Always suspend initially. The scheduler will resume when ready.
         * This allows proper scheduling and lifecycle management.
         */
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        /**
         * @brief Final suspension point
         *
         * Resumes the continuation if one exists, then destroys this coroutine.
         * Uses symmetric transfer for optimal performance.
         */
        auto final_suspend() noexcept {
            struct final_awaiter {
                std::coroutine_handle<> continuation_;

                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
                    // Symmetric transfer: return the continuation handle directly
                    // This avoids stack overflow in deep coroutine chains
                    return continuation_ ? continuation_ : std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };
            return final_awaiter{continuation_};
        }

        /**
         * @brief Handle unhandled exceptions
         *
         * Stores the exception for rethrowing in await_resume().
         */
        void unhandled_exception() noexcept {
            result_.template emplace<2>(std::current_exception());
        }

        /**
         * @brief Store the return value
         * @param value The value to return
         */
        void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
            result_.template emplace<1>(std::move(value));
        }

        /**
         * @brief Check if the coroutine has completed
         * @return true if result is available (success or exception)
         */
        [[nodiscard]] bool is_ready() const noexcept {
            return result_.index() != 0;
        }

        /**
         * @brief Check if the coroutine completed with an exception
         * @return true if an exception was thrown
         */
        [[nodiscard]] bool has_exception() const noexcept {
            return result_.index() == 2;
        }

        /**
         * @brief Get the return value
         * @return Reference to the stored value
         * @pre is_ready() && !has_exception()
         */
        T& value() noexcept {
            return std::get<1>(result_);
        }

        /**
         * @brief Get the stored exception
         * @return Reference to the exception pointer
         * @pre has_exception()
         */
        std::exception_ptr& exception() noexcept {
            return std::get<2>(result_);
        }

#ifdef QB_DEBUG_COROUTINES
        ~promise_type() {
            QB_CORO_TRACE(coro_id_, "promise_destroyed");
        }
#endif
    };

    using handle_type = std::coroutine_handle<promise_type>;

    /**
     * @brief Construct from a coroutine handle
     * @param h The coroutine handle
     * @private
     */
    explicit task(handle_type h) noexcept : handle_(h) {}

    /**
     * @brief Destructor
     *
     * Destroys the coroutine frame. The task must not be in the ready queue.
     */
    ~task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /**
     * @brief Move constructor
     */
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    /**
     * @brief Move assignment
     */
    task& operator=(task&& other) noexcept {
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    // Non-copyable
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    /**
     * @brief Awaitable: check if ready
     * @return true if coroutine is complete
     */
    [[nodiscard]] bool await_ready() const noexcept {
        if (!handle_) return true;
        if (handle_.done()) return true;
        return handle_.promise().is_ready();
    }

    /**
     * @brief Awaitable: suspend and set continuation
     * @param caller The coroutine awaiting this task
     * @return Handle to resume (symmetric transfer)
     *
     * Sets the continuation and returns this task's handle for symmetric transfer.
     * The continuation will be resumed when this coroutine completes via final_suspend.
     *
     * IMPLEMENTATION NOTE: This is defined inline to avoid circular dependency
     * with CoroutineScheduler. Symmetric transfer prevents stack overflow.
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        // Symmetric transfer: return the handle to resume
        // The compiler will resume it directly without recursion
        return handle_;
    }

    /**
     * @brief Awaitable: get the result
     * @return The return value
     * @throws Any exception thrown by the coroutine
     */
    T await_resume() {
        if (handle_.promise().has_exception()) {
            std::rethrow_exception(handle_.promise().exception());
        }
        // Force a copy/move into a local variable before returning
        // This ensures the value is extracted before the task destructor runs
        T result = std::move(handle_.promise().value());
        return result;
    }

    /**
     * @brief Get the coroutine handle
     * @return The handle (may be null)
     */
    [[nodiscard]] handle_type handle() const noexcept { return handle_; }

    /**
     * @brief Detach the coroutine handle from this task
     * @return The handle (may be null)
     *
     * This transfers ownership of the handle to the caller.
     * The task will no longer own the handle and will not
     * destroy it in its destructor.
     */
    [[nodiscard]] handle_type detach() noexcept { return std::exchange(handle_, {}); }

    /**
     * @brief Check if the coroutine has completed
     * @return true if done
     */
    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

    /**
     * @brief Check if the coroutine is valid
     * @return true if handle is not null
     */
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    handle_type handle_;
};

/**
 * @brief Specialization for void return type
 * @ingroup Coroutine
 */
template <>
class task<void> {
public:
    using value_type = void;

    /**
     * @brief Promise type for task<void>
     */
    struct promise_type {
        /**
         * @brief Stored exception (if any)
         */
        std::exception_ptr exception_;

        /**
         * @brief Continuation for coroutine composition
         */
        std::coroutine_handle<> continuation_;

        /**
         * @brief Scheduler pointer
         */
        CoroutineScheduler* scheduler_ = nullptr;

        /**
         * @brief Completion flag
         */
        bool completed_ = false;

#ifdef QB_DEBUG_COROUTINES
        static inline std::atomic<std::size_t> next_id{0};
        std::size_t coro_id_ = next_id.fetch_add(1, std::memory_order_relaxed);
#endif

        task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept {
#ifdef QB_DEBUG_COROUTINES
            QB_CORO_TRACE(coro_id_, "initial_suspend");
#endif
            return {};
        }

        auto final_suspend() noexcept {
            struct final_awaiter {
                std::coroutine_handle<> continuation_;
#ifdef QB_DEBUG_COROUTINES
                std::size_t coro_id_;
#endif

                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
#ifdef QB_DEBUG_COROUTINES
                    QB_CORO_TRACE(coro_id_, "final_suspend");
#endif
                    // Symmetric transfer for optimal performance
                    return continuation_ ? continuation_ : std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };
            return final_awaiter{continuation_
#ifdef QB_DEBUG_COROUTINES
                , coro_id_
#endif
            };
        }

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }

        void return_void() noexcept {
            completed_ = true;
        }

        [[nodiscard]] bool is_ready() const noexcept {
            return completed_ || exception_ != nullptr;
        }

        [[nodiscard]] bool has_exception() const noexcept {
            return exception_ != nullptr;
        }

#ifdef QB_DEBUG_COROUTINES
        ~promise_type() {
            QB_CORO_TRACE(coro_id_, "promise_destroyed");
        }
#endif
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) noexcept : handle_(h) {}

    ~task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    task& operator=(task&& other) noexcept {
        if (handle_) {
            handle_.destroy();
        }
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        if (!handle_) return true;
        if (handle_.done()) return true;
        return handle_.promise().is_ready();
    }

    /**
     * @brief Awaitable: suspend and set continuation
     * @param caller The coroutine awaiting this task
     * @return Handle to resume (symmetric transfer)
     *
     * Sets the continuation and returns this task's handle for symmetric transfer.
     * The continuation will be resumed when this coroutine completes via final_suspend.
     *
     * IMPLEMENTATION NOTE: This is defined inline to avoid circular dependency
     * with CoroutineScheduler. Symmetric transfer prevents stack overflow.
     */
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        // Symmetric transfer: return the handle to resume
        return handle_;
    }

    void await_resume() {
        if (handle_.promise().has_exception()) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }

    /**
     * @brief Detach the coroutine handle from this task
     * @return The handle (may be null)
     *
     * This transfers ownership of the handle to the caller.
     * The task will no longer own the handle and will not
     * destroy it in its destructor.
     */
    [[nodiscard]] handle_type detach() noexcept { return std::exchange(handle_, {}); }

    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    handle_type handle_;
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_TASK_H
