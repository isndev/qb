/**
 * @file qb/io/async/coroutine/retry.h
 * @brief Retry utilities for coroutines
 *
 * Provides retry mechanisms with various backoff strategies.
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

#ifndef QB_IO_ASYNC_COROUTINE_RETRY_H
#define QB_IO_ASYNC_COROUTINE_RETRY_H

#include "task.h"
#include "utils.h"
#include <qb/system/timestamp.h> // qb::duration
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

namespace qb::io::async {

/**
 * @brief Exception thrown when all retry attempts are exhausted
 */
class retry_exhausted : public std::runtime_error {
    size_t             _attempts;
    std::exception_ptr _last_error;

public:
    retry_exhausted(size_t attempts, std::exception_ptr last_error)
        : std::runtime_error("All retry attempts exhausted")
        , _attempts(attempts)
        , _last_error(last_error) {}

    size_t
    attempts() const {
        return _attempts;
    }
    std::exception_ptr
    last_error() const {
        return _last_error;
    }

    void
    rethrow_last() const {
        if (_last_error) {
            std::rethrow_exception(_last_error);
        }
    }
};

/**
 * @brief Backoff strategy types
 */
enum class backoff_strategy {
    fixed,             // Constant delay
    linear,            // Delay increases linearly
    exponential,       // Delay doubles each retry
    exponential_jitter // Exponential with random jitter
};

/**
 * @brief Retry policy configuration
 */
struct retry_policy {
    size_t           max_attempts = 3;
    qb::duration     base_delay{std::chrono::milliseconds{100}};
    qb::duration     max_delay{std::chrono::milliseconds{30'000}}; // 30 seconds
    backoff_strategy strategy = backoff_strategy::exponential;

    // Predicate to determine if an error is retryable
    std::function<bool(const std::exception &)> is_retryable = [](const std::exception &) {
        return true;
    };

    // Optional callback for each retry
    std::function<void(size_t attempt, const std::exception &)> on_retry = nullptr;
};

namespace detail {

/**
 * @brief Compute the backoff delay for the Nth retry (1-based).
 *
 * @param retry_number 1-based retry number (1 = first retry after the first
 *                     failure, 2 = second retry, …). Callers must pass at
 *                     least 1; values < 1 are clamped up.
 * @param policy       Retry policy describing strategy, base/max delay.
 *
 * Finding 2.C.2: the original implementation accepted a 0-based attempt
 * counter, which made the linear strategy produce a 0 ms delay for the
 * first retry (tight spin). Using a 1-based counter here gives:
 *   - fixed:       base_delay
 *   - linear:      base_delay * retry_number
 *   - exponential: base_delay * 2^(retry_number-1)
 *
 * Finding 2.C.13: compute the exponential scale in a 64-bit integer to
 * avoid overflow before the `max_delay` clamp (otherwise `base_delay *
 * (1u << 30)` silently overflows `chrono::milliseconds`' signed rep).
 */
inline qb::duration
calculate_delay(size_t retry_number, const retry_policy &policy) {
    using ms_rep = std::chrono::milliseconds::rep;
    if (retry_number < 1)
        retry_number = 1;

    // The backoff/jitter math is deliberately computed in millisecond-resolution
    // integers; the policy fields are qb::duration (nanoseconds), so cast in.
    const ms_rep base_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(policy.base_delay).count();
    const ms_rep max_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(policy.max_delay).count();
    ms_rep       delay_ms = base_ms;

    switch (policy.strategy) {
        case backoff_strategy::fixed:
            delay_ms = base_ms;
            break;

        case backoff_strategy::linear: {
            // Clamp multiplier before multiplying to avoid overflow on
            // pathological inputs.
            const ms_rep mult = static_cast<ms_rep>(std::min<size_t>(retry_number, 10000));
            if (base_ms != 0 && mult > (std::numeric_limits<ms_rep>::max() / base_ms))
                delay_ms = max_ms; // overflow → clamp
            else
                delay_ms = base_ms * mult;
            break;
        }

        case backoff_strategy::exponential: {
            // retry_number is 1-based: first retry uses shift 0 = base_delay.
            const size_t shift  = std::min<size_t>(retry_number - 1, 30);
            const ms_rep factor = static_cast<ms_rep>(1ULL << shift);
            if (base_ms != 0 && factor > (std::numeric_limits<ms_rep>::max() / base_ms))
                delay_ms = max_ms;
            else
                delay_ms = base_ms * factor;
            break;
        }

        case backoff_strategy::exponential_jitter: {
            const size_t shift  = std::min<size_t>(retry_number - 1, 30);
            const ms_rep factor = static_cast<ms_rep>(1ULL << shift);
            if (base_ms != 0 && factor > (std::numeric_limits<ms_rep>::max() / base_ms))
                delay_ms = max_ms;
            else
                delay_ms = base_ms * factor;
            // 0-50% jitter using thread-local RNG (single-thread per worker,
            // but `thread_local` is still correct and makes the API work from
            // any context — e.g. tests).
            {
                // Clamp to max BEFORE the jitter multiply so `delay_ms * 49`
                // cannot overflow when the pre-jitter delay is already huge.
                if (delay_ms > max_ms)
                    delay_ms = max_ms;
                static thread_local std::mt19937      rng{std::random_device{}()};
                std::uniform_int_distribution<ms_rep> dist(0, 49);
                const ms_rep                          jitter_pct = dist(rng);
                if (jitter_pct != 0 && delay_ms > (std::numeric_limits<ms_rep>::max)() / jitter_pct / 2)
                    delay_ms = max_ms; // would overflow → saturate
                else
                    delay_ms += (delay_ms * jitter_pct) / 100;
            }
            break;
        }
    }

    if (delay_ms > max_ms)
        delay_ms = max_ms;
    if (delay_ms < 0)
        delay_ms = 0;
    return qb::duration{std::chrono::milliseconds{delay_ms}};
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
auto
with_retry(F f, retry_policy policy = {}) -> task<typename std::invoke_result_t<F>::value_type>
requires(!std::same_as<typename std::invoke_result_t<F>::value_type, void>)
{
    using task_type   = std::invoke_result_t<F>;
    using result_type = typename task_type::value_type;

    std::exception_ptr last_error;
    size_t             current_attempt = 0;

    while (current_attempt < policy.max_attempts) {
        // Try the operation
        std::optional<result_type> result;
        bool                       success = false;

        try {
            result  = co_await f();
            success = true;
        } catch (const std::exception &e) {
            last_error = std::current_exception();

            // Check if error is retryable
            if (!policy.is_retryable(e)) {
                std::rethrow_exception(last_error);
            }

            // Notify callback if set
            if (policy.on_retry) {
                policy.on_retry(current_attempt + 1, e);
            }
        } catch (...) {
            // Finding 2.C.14: do not let non-std::exception throwables bypass
            // retry bookkeeping — capture, notify, and retry exactly as we do
            // for std::exception. The `is_retryable` predicate can only see
            // std::exception, so for unknown exception types we conservatively
            // attempt a retry.
            last_error = std::current_exception();
        }

        if (success) {
            co_return std::move(*result);
        }

        // Don't retry after last attempt
        ++current_attempt;
        if (current_attempt >= policy.max_attempts) {
            break;
        }

        // Wait before retry. `current_attempt` is 1-based here
        // (1 after first failure, 2 after second, …) which matches
        // `calculate_delay`'s post-fix contract (Finding 2.C.2).
        auto delay = detail::calculate_delay(current_attempt, policy);
        co_await sleep(delay);
    }

    throw retry_exhausted(policy.max_attempts, last_error);
}

/**
 * @brief Retry a coroutine operation that returns void
 * @tparam F Coroutine function type returning task<void>
 */
template <typename F>
auto
with_retry(F f, retry_policy policy = {}) -> task<void>
requires std::same_as<std::invoke_result_t<F>, task<void>>
{
    std::exception_ptr last_error;
    size_t             current_attempt = 0;

    while (current_attempt < policy.max_attempts) {
        bool success = false;
        try {
            co_await f();
            success = true;
        } catch (const std::exception &e) {
            last_error = std::current_exception();
            if (!policy.is_retryable(e)) {
                std::rethrow_exception(last_error);
            }
            if (policy.on_retry) {
                policy.on_retry(current_attempt + 1, e);
            }
        } catch (...) {
            // Finding 2.C.14: see sibling overload above.
            last_error = std::current_exception();
        }

        if (success) {
            co_return;
        }

        ++current_attempt;
        if (current_attempt >= policy.max_attempts) {
            break;
        }

        // current_attempt is now 1-based (Finding 2.C.2).
        auto delay = detail::calculate_delay(current_attempt, policy);
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
auto
with_retry_until(F f, P is_success, retry_policy policy = {}) -> task<typename std::invoke_result_t<F>::value_type> {
    using task_type   = std::invoke_result_t<F>;
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
                const char *
                what() const noexcept override {
                    return "Unsuccessful result";
                }
            } e;
            policy.on_retry(attempt + 1, e);
        }

        // `attempt` is 0-based here; `calculate_delay` expects 1-based
        // (Finding 2.C.2).
        auto delay = detail::calculate_delay(attempt + 1, policy);
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
auto
retry(F f) -> task<typename std::invoke_result_t<F>::value_type> {
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
auto
make_retryable(F f, retry_policy policy = {}) {
    return [f = std::move(f), policy = std::move(policy)]() mutable -> task<typename std::invoke_result_t<F>::value_type> {
        co_return co_await with_retry(f, policy);
    };
}

/**
 * @brief Policy for transient network errors
 */
inline retry_policy
transient_network_policy() {
    return retry_policy{
        .max_attempts = 5,
        .base_delay   = std::chrono::milliseconds(100),
        .max_delay    = std::chrono::seconds(30),
        .strategy     = backoff_strategy::exponential_jitter,
        .is_retryable = [](const std::exception &e) {
            std::string msg = e.what();
            return msg.find("timeout") != std::string::npos || msg.find("connection") != std::string::npos
                   || msg.find("reset") != std::string::npos || msg.find("temporarily") != std::string::npos;
        }
    };
}

/**
 * @brief Policy for idempotent operations (safe to retry)
 */
inline retry_policy
idempotent_policy() {
    return retry_policy{
        .max_attempts = 10,
        .base_delay   = std::chrono::milliseconds(50),
        .max_delay    = std::chrono::seconds(60),
        .strategy     = backoff_strategy::exponential_jitter,
        .is_retryable = [](const std::exception &) { return true; }
    };
}

/**
 * @brief Policy for aggressive fast retry
 */
inline retry_policy
aggressive_retry_policy() {
    return retry_policy{
        .max_attempts = 20,
        .base_delay   = std::chrono::milliseconds(10),
        .max_delay    = std::chrono::seconds(5),
        .strategy     = backoff_strategy::linear,
        .is_retryable = [](const std::exception &) { return true; }
    };
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_RETRY_H
