/**
 * @file qb/io/async/coroutine/retry.h
 * @brief Retry utilities for coroutines
 *
 * Provides retry mechanisms with various backoff strategies.
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

#ifndef QB_IO_ASYNC_COROUTINE_RETRY_H
#define QB_IO_ASYNC_COROUTINE_RETRY_H

#include "task.h"
#include "utils.h"
#include <functional>
#include <exception>
#include <optional>
#include <random>

namespace qb::io::async {

/**
 * @brief Exception thrown when all retry attempts are exhausted
 */
class retry_exhausted : public std::runtime_error {
    size_t _attempts;
    std::exception_ptr _last_error;

public:
    retry_exhausted(size_t attempts, std::exception_ptr last_error)
        : std::runtime_error("All retry attempts exhausted")
        , _attempts(attempts)
        , _last_error(last_error) {}

    size_t attempts() const { return _attempts; }
    std::exception_ptr last_error() const { return _last_error; }

    void rethrow_last() const {
        if (_last_error) {
            std::rethrow_exception(_last_error);
        }
    }
};

/**
 * @brief Backoff strategy types
 */
enum class backoff_strategy {
    fixed,          // Constant delay
    linear,         // Delay increases linearly
    exponential,    // Delay doubles each retry
    exponential_jitter  // Exponential with random jitter
};

/**
 * @brief Retry policy configuration
 */
struct retry_policy {
    size_t max_attempts = 3;
    std::chrono::milliseconds base_delay{100};
    std::chrono::milliseconds max_delay{30'000};  // 30 seconds
    backoff_strategy strategy = backoff_strategy::exponential;

    // Predicate to determine if an error is retryable
    std::function<bool(const std::exception&)> is_retryable = [](const std::exception&) {
        return true;
    };

    // Optional callback for each retry
    std::function<void(size_t attempt, const std::exception&)> on_retry = nullptr;
};

namespace detail {

inline std::chrono::milliseconds calculate_delay(
    size_t attempt,
    const retry_policy& policy) {

    std::chrono::milliseconds delay = policy.base_delay;

    switch (policy.strategy) {
        case backoff_strategy::fixed:
            delay = policy.base_delay;
            break;

        case backoff_strategy::linear:
            delay = policy.base_delay * static_cast<long long>(std::min<size_t>(attempt, 10000));
            break;

        case backoff_strategy::exponential: {
            auto shift = std::min<size_t>(attempt, 30);
            delay = policy.base_delay * (1u << shift);
            break;
        }

        case backoff_strategy::exponential_jitter: {
            auto shift = std::min<size_t>(attempt, 30);
            delay = policy.base_delay * (1u << shift);
            // Add 0-50% jitter using thread-local RNG
            {
                static thread_local std::mt19937 rng{std::random_device{}()};
                std::uniform_int_distribution<long long> dist(0, 49);
                delay += std::chrono::milliseconds(
                    (delay.count() * dist(rng)) / 100);
            }
            break;
        }
    }

    // Cap at max delay
    if (delay > policy.max_delay) {
        delay = policy.max_delay;
    }

    return delay;
}

} // namespace detail

/**
 * @brief Retry a coroutine operation with backoff
 * @tparam F Coroutine function type
 * @param f Function returning a task<T>
 * @param policy Retry configuration
 * @return Task that completes with result or throws retry_exhausted
 *
 * Usage:
 * @code
 * auto result = co_await with_retry(
 *     []() -> task<std::string> {
 *         co_return co_await fetch_data();
 *     },
 *     retry_policy{
 *         .max_attempts = 5,
 *         .base_delay = 100ms,
 *         .strategy = backoff_strategy::exponential
 *     }
 * );
 * @endcode
 */
template <typename F>
auto with_retry(F f, retry_policy policy = {})
    -> task<typename std::invoke_result_t<F>::value_type>
    requires (!std::same_as<typename std::invoke_result_t<F>::value_type, void>) {
    using task_type = std::invoke_result_t<F>;
    using result_type = typename task_type::value_type;

    std::exception_ptr last_error;
    size_t current_attempt = 0;

    while (current_attempt < policy.max_attempts) {
        // Try the operation
        std::optional<result_type> result;
        bool success = false;

        try {
            result = co_await f();
            success = true;
        } catch (const std::exception& e) {
            last_error = std::current_exception();

            // Check if error is retryable
            if (!policy.is_retryable(e)) {
                std::rethrow_exception(last_error);
            }

            // Notify callback if set
            if (policy.on_retry) {
                policy.on_retry(current_attempt + 1, e);
            }
        }

        if (success) {
            co_return std::move(*result);
        }

        // Don't retry after last attempt
        ++current_attempt;
        if (current_attempt >= policy.max_attempts) {
            break;
        }

        // Wait before retry
        auto delay = detail::calculate_delay(current_attempt - 1, policy);
        co_await sleep(delay);
    }

    throw retry_exhausted(policy.max_attempts, last_error);
}

/**
 * @brief Retry a coroutine operation that returns void
 * @tparam F Coroutine function type returning task<void>
 */
template <typename F>
auto with_retry(F f, retry_policy policy = {}) -> task<void>
    requires std::same_as<std::invoke_result_t<F>, task<void>> {
    std::exception_ptr last_error;
    size_t current_attempt = 0;

    while (current_attempt < policy.max_attempts) {
        bool success = false;
        try {
            co_await f();
            success = true;
        } catch (const std::exception& e) {
            last_error = std::current_exception();
            if (!policy.is_retryable(e)) {
                std::rethrow_exception(last_error);
            }
            if (policy.on_retry) {
                policy.on_retry(current_attempt + 1, e);
            }
        }

        if (success) {
            co_return;
        }

        ++current_attempt;
        if (current_attempt >= policy.max_attempts) {
            break;
        }

        auto delay = detail::calculate_delay(current_attempt - 1, policy);
        co_await sleep(delay);
    }

    throw retry_exhausted(policy.max_attempts, last_error);
}

/**
 * @brief Retry with result predicate
 * @tparam F Function type
 * @tparam P Predicate type for checking success
 * @param f Function to retry
 * @param is_success Predicate that returns true if result is successful
 * @param policy Retry configuration
 * @return Task with result
 *
 * Usage:
 * @code
 * // Retry until response is not empty
 * auto response = co_await with_retry_until(
 *     []() -> task<std::string> {
 *         co_return co_await query_service();
 *     },
 *     [](const std::string& r) { return !r.empty(); },
 *     retry_policy{.max_attempts = 10}
 * );
 * @endcode
 */
template <typename F, typename P>
auto with_retry_until(F f, P is_success, retry_policy policy = {})
    -> task<typename std::invoke_result_t<F>::value_type> {
    using task_type = std::invoke_result_t<F>;
    using result_type = typename task_type::value_type;

    for (size_t attempt = 0; attempt < policy.max_attempts; ++attempt) {
        result_type value = co_await f();

        if (is_success(value)) {
            co_return value;
        }

        // Result not successful - retry
        if (attempt + 1 >= policy.max_attempts) {
            break;
        }

        if (policy.on_retry) {
            struct dummy_exception : std::exception {
                const char* what() const noexcept override {
                    return "Unsuccessful result";
                }
            } e;
            policy.on_retry(attempt + 1, e);
        }

        auto delay = detail::calculate_delay(attempt, policy);
        co_await sleep(delay);
    }

    throw retry_exhausted(policy.max_attempts, nullptr);
}

/**
 * @brief Retry with default policy (3 attempts, exponential backoff)
 * @tparam F Function type
 * @param f Function to retry
 * @return Task with result
 * @ingroup Coroutine
 */
template <typename F>
auto retry(F f) -> task<typename std::invoke_result_t<F>::value_type> {
    return with_retry(std::move(f), retry_policy{});
}

/**
 * @brief Create a retryable wrapper for a function
 * @tparam F Function type
 * @param f Function to wrap
 * @param policy Retry policy (captured by value)
 * @return Wrapped function that retries automatically
 *
 * Usage:
 * @code
 * auto fetch_with_retry = make_retryable(
 *     []() -> task<std::string> { co_return co_await fetch(); },
 *     retry_policy{.max_attempts = 5}
 * );
 *
 * // Later - automatic retry
 * auto result = co_await fetch_with_retry();
 * @endcode
 */
template <typename F>
auto make_retryable(F f, retry_policy policy = {}) {
    return [f = std::move(f), policy = std::move(policy)]() mutable
               -> task<typename std::invoke_result_t<F>::value_type> {
        co_return co_await with_retry(f, policy);
    };
}

/**
 * @brief Policy for transient network errors
 */
inline retry_policy transient_network_policy() {
    return retry_policy{
        .max_attempts = 5,
        .base_delay = std::chrono::milliseconds(100),
        .max_delay = std::chrono::seconds(30),
        .strategy = backoff_strategy::exponential_jitter,
        .is_retryable = [](const std::exception& e) {
            std::string msg = e.what();
            return msg.find("timeout") != std::string::npos ||
                   msg.find("connection") != std::string::npos ||
                   msg.find("reset") != std::string::npos ||
                   msg.find("temporarily") != std::string::npos;
        }
    };
}

/**
 * @brief Policy for idempotent operations (safe to retry)
 */
inline retry_policy idempotent_policy() {
    return retry_policy{
        .max_attempts = 10,
        .base_delay = std::chrono::milliseconds(50),
        .max_delay = std::chrono::seconds(60),
        .strategy = backoff_strategy::exponential_jitter,
        .is_retryable = [](const std::exception&) { return true; }
    };
}

/**
 * @brief Policy for aggressive fast retry
 */
inline retry_policy aggressive_retry_policy() {
    return retry_policy{
        .max_attempts = 20,
        .base_delay = std::chrono::milliseconds(10),
        .max_delay = std::chrono::seconds(5),
        .strategy = backoff_strategy::linear,
        .is_retryable = [](const std::exception&) { return true; }
    };
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_RETRY_H
