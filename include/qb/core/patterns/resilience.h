/**
 * @file qb/core/patterns/resilience.h
 * @brief Resilient `ask`: retry with exponential backoff, and a circuit breaker.
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
 * @ingroup Patterns
 */

#ifndef QB_CORE_PATTERNS_RESILIENCE_H
#define QB_CORE_PATTERNS_RESILIENCE_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <qb/io/async/coroutine.h>
#include <qb/system/time.h> // qb::duration
#include "request.h"

namespace qb {

/**
 * @struct retry_policy
 * @ingroup Patterns
 * @brief Retry-with-backoff configuration for `qb::ask_retry`.
 * @details Exponential backoff: the wait before retry `n` is `min(backoff * multiplier^(n-1),
 *          max_backoff)`. A retried `ask` that keeps timing out throws `timeout_error` after
 *          `max_attempts` tries.
 */
struct retry_policy {
    int          max_attempts = 3;                            ///< Total tries (>= 1).
    qb::duration backoff      = std::chrono::milliseconds(50); ///< Wait before the first retry.
    double       multiplier   = 2.0;                          ///< Exponential growth factor (>= 1).
    qb::duration max_backoff  = std::chrono::seconds(1);      ///< Upper bound on a single backoff.
};

/**
 * @struct circuit_open_error
 * @ingroup Patterns
 * @brief Thrown by `qb::ask_guarded` when the `CircuitBreaker` is open (the request fails fast).
 */
struct circuit_open_error : std::runtime_error {
    circuit_open_error()
        : std::runtime_error("qb::CircuitBreaker: circuit is open") {}
};

/**
 * @class CircuitBreaker
 * @ingroup Patterns
 * @brief A three-state circuit breaker (closed / open / half-open) for resilient calls.
 * @details
 * Trips **open** after `failure_threshold` consecutive failures, then fails fast for a `cooldown`
 * window; afterwards it admits a **half-open** trial — a success closes it, a failure re-opens it.
 * It is a plain single-thread state machine (no timer of its own): the caller drives it with the
 * `VirtualCore` timestamp (`ctx.time()`). Hold it by `std::shared_ptr` so a coroutine can capture
 * it **by value** and outlive its actor — see `qb::ask_guarded`.
 * @note Half-open admits trial calls until the next result resolves the state; in a typical
 *       single-coroutine flow that is exactly one trial.
 */
class CircuitBreaker {
public:
    /** @brief Breaker state. */
    enum class State { closed, open, half_open };

    /**
     * @brief Construct a breaker.
     * @param failure_threshold Consecutive failures that trip the breaker (clamped to >= 1).
     * @param cooldown How long to fail fast before admitting a half-open trial.
     */
    CircuitBreaker(unsigned failure_threshold, qb::duration cooldown) noexcept
        : _threshold(failure_threshold ? failure_threshold : 1u)
        , _cooldown(cooldown) {}

    /**
     * @brief Whether a call may proceed now; transitions open -> half-open after cooldown.
     * @param now_ns Current `VirtualCore` timestamp in nanoseconds (e.g. `ctx.time()`).
     * @return `true` if the call may proceed (closed, or a half-open trial); `false` to fail fast.
     */
    [[nodiscard]] bool
    allow(uint64_t now_ns) noexcept {
        if (_state == State::open) {
            if (now_ns - _opened_at >= static_cast<uint64_t>(_cooldown.count())) {
                _state = State::half_open; // admit a trial
                return true;
            }
            return false; // still cooling down
        }
        return true; // closed, or half-open trial already admitted
    }

    /** @brief Record a successful call: reset to closed. */
    void
    on_success() noexcept {
        _state    = State::closed;
        _failures = 0;
    }

    /**
     * @brief Record a failed call: open the breaker if the threshold is reached (or a half-open
     *        trial failed).
     * @param now_ns Current `VirtualCore` timestamp in nanoseconds.
     */
    void
    on_failure(uint64_t now_ns) noexcept {
        if (_state == State::half_open || ++_failures >= _threshold) {
            _state     = State::open;
            _opened_at = now_ns;
        }
    }

    /** @brief Current state. */
    [[nodiscard]] State
    state() const noexcept {
        return _state;
    }

    /** @brief Consecutive failures recorded while closed. */
    [[nodiscard]] unsigned
    failure_count() const noexcept {
        return _failures;
    }

private:
    unsigned     _threshold;
    qb::duration _cooldown;
    State        _state     = State::closed;
    unsigned     _failures  = 0;
    uint64_t     _opened_at = 0;
};

/** @brief Alias for `CircuitBreaker`. */
using circuit_breaker = CircuitBreaker;

/**
 * @brief `ask` with automatic retry and exponential backoff on timeout.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param target The actor to ask.
 * @param req The request (re-sent, copied, on each attempt with a fresh correlation id).
 * @param timeout Per-attempt timeout.
 * @param policy Retry/backoff configuration.
 * @return `task<E>` resolving to the first successful reply.
 * @throws qb::io::async::timeout_error if all `policy.max_attempts` attempts time out.
 * @throws qb::io::async::cancelled_error if the actor is killed (retries abort immediately).
 * @details Only timeouts are retried; a kill propagates at once. Backoff waits are
 *          cancellation-aware (`ctx.sleep`), so a kill during a backoff also aborts.
 * @code
 * auto r = co_await qb::ask_retry(ctx, market, Quote{"BTC"}, 200ms, {.max_attempts = 5});
 * @endcode
 * @see qb::ask, qb::retry_policy
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_retry(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout,
          qb::retry_policy policy = {}) {
    qb::duration backoff = policy.backoff;
    for (int attempt = 1;; ++attempt) {
        try {
            co_return co_await qb::ask<E>(ctx, target, req, timeout);
        } catch (const qb::io::async::timeout_error &) {
            if (attempt >= policy.max_attempts)
                throw; // attempts exhausted — propagate the timeout
        }
        // `co_await` is illegal in a catch handler, so back off here (cancellation-aware).
        co_await ctx.sleep(backoff);
        const auto grown =
            static_cast<qb::duration::rep>(static_cast<double>(backoff.count()) * policy.multiplier);
        backoff = (std::min)(qb::duration{grown}, policy.max_backoff);
    }
}

/**
 * @brief `ask` guarded by a `CircuitBreaker`: fail fast when open, record the outcome.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param breaker Shared breaker (capture it by value into the coroutine so it outlives the actor).
 * @param target The actor to ask.
 * @param req The request.
 * @param timeout Timeout for the underlying `ask`.
 * @return `task<E>` resolving to the reply when the breaker is closed/half-open and the ask succeeds.
 * @throws qb::circuit_open_error immediately (no request sent) if the breaker is open.
 * @throws qb::io::async::timeout_error on a timed-out ask — also recorded as a breaker failure.
 * @throws qb::io::async::cancelled_error on kill — **not** counted as a breaker failure.
 * @details A success closes the breaker; a timeout (or other non-cancellation error) is a failure
 *          that may trip it. Compose with `ask_retry` by retrying around this call.
 * @see CircuitBreaker, qb::ask, qb::ask_retry
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_guarded(qb::ScopedCoroContext ctx, std::shared_ptr<qb::CircuitBreaker> breaker,
            qb::ActorId target, E req, qb::duration timeout) {
    if (!breaker->allow(ctx.time()))
        throw qb::circuit_open_error{}; // fail fast — the request is never sent

    std::exception_ptr failure;
    try {
        E resp = co_await qb::ask<E>(ctx, target, req, timeout);
        breaker->on_success();
        co_return resp;
    } catch (const qb::io::async::cancelled_error &) {
        throw; // actor killed — not a breaker failure
    } catch (...) {
        failure = std::current_exception();
    }
    breaker->on_failure(ctx.time());
    std::rethrow_exception(failure);
}

} // namespace qb

#endif // QB_CORE_PATTERNS_RESILIENCE_H
