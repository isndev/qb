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
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
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
    int          max_attempts = 3;                             ///< Total tries (>= 1).
    qb::duration backoff      = std::chrono::milliseconds(50); ///< Wait before the first retry.
    double       multiplier   = 2.0;                           ///< Exponential growth factor (>= 1).
    qb::duration max_backoff  = std::chrono::seconds(1);       ///< Upper bound on a single backoff.
    /**
     * @brief Randomization fraction in `[0, 1]` applied to each backoff (default `0` = none).
     * @details The actual wait is drawn uniformly from `[backoff * (1 - jitter), backoff]`, so
     *          `jitter = 0.2` spreads retries over the last 20 % of the interval. Jitter
     *          desynchronizes many clients retrying at once (avoids retry-storms). For the generic,
     *          non-`ask` case `qb::io::async::with_retry` / `retry_policy` (retry.h) offers the same
     *          via `backoff_strategy::exponential_jitter`.
     */
    double jitter = 0.0;
};

namespace detail {
/// Apply `policy.jitter` to a backoff: uniform in `[d*(1-jitter), d]`. Thread-local RNG
/// (mono-thread per VirtualCore; thread_local keeps it correct from any context).
[[nodiscard]] inline qb::duration
apply_retry_jitter(qb::duration d, double jitter) noexcept {
    if (jitter <= 0.0)
        return d;
    jitter = (std::min) (jitter, 1.0);
    static thread_local std::mt19937_64    rng{std::random_device{}()};
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double                           factor = 1.0 - jitter * u(rng); // in [1 - jitter, 1]
    const auto                             out    = static_cast<qb::duration::rep>(static_cast<double>(d.count()) * factor);
    return qb::duration{out < 0 ? qb::duration::rep{0} : out};
}
} // namespace detail

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
        // Clamp a negative cooldown to zero: `allow()` compares an unsigned ns delta against
        // `_cooldown.count()`, and a negative count cast to uint64_t becomes ~1.8e19 ns, which
        // would make the breaker never recover.
        , _cooldown(cooldown.count() < 0 ? qb::duration::zero() : cooldown) {}

    /**
     * @brief Whether a call may proceed now; transitions open -> half-open after cooldown.
     * @param now_ns Current `VirtualCore` timestamp in nanoseconds (e.g. `ctx.time()`).
     * @return `true` if the call may proceed (closed, or the single half-open trial); `false`
     *         to fail fast (open and still cooling down, or a half-open trial already in flight).
     * @details Half-open admits **exactly one** trial: the call that triggers the open->half-open
     *          transition. Concurrent callers sharing the breaker fail fast while that trial is in
     *          flight (no thundering-herd against a still-down dependency). The trial must resolve
     *          the state via `on_success` / `on_failure`, or release it via `on_abandoned` if its
     *          caller is killed — otherwise the breaker would stay half-open and reject everything.
     */
    [[nodiscard]] bool
    allow(uint64_t now_ns) noexcept {
        if (_state == State::open) {
            if (now_ns - _opened_at >= static_cast<uint64_t>(_cooldown.count())) {
                _state = State::half_open; // admit exactly one trial
                return true;
            }
            return false; // still cooling down
        }
        if (_state == State::half_open)
            return false; // a trial is already in flight — fail fast until it resolves
        return true;      // closed
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

    /**
     * @brief Release a half-open trial that was abandoned without a verdict (e.g. its caller was
     *        killed mid-`ask`), re-arming the cooldown so a later call gets a fresh trial.
     * @param now_ns Current `VirtualCore` timestamp in nanoseconds.
     * @details Without this, a single-trial half-open whose trial is cancelled (no `on_success` /
     *          `on_failure`) would wedge the breaker rejecting every subsequent call. No-op unless
     *          half-open.
     */
    void
    on_abandoned(uint64_t now_ns) noexcept {
        if (_state == State::half_open) {
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
 * @class rate_limiter
 * @ingroup Patterns
 * @brief A token-bucket rate limiter for coroutines (flow control / throttling).
 * @details
 * Starts full with `capacity` tokens; one token regenerates every `per_token`. `acquire(ctx)`
 * consumes a token, **waiting** (cancellation-aware via `ctx.sleep`) when the bucket is empty;
 * `try_acquire(now_ns)` is the non-blocking probe. It is a plain single-thread state machine (no
 * timer of its own) driven by the `VirtualCore` clock (`ctx.time()` / `now_ns`). Hold it by
 * `std::shared_ptr` to share one limiter across coroutines that outlive their actor — like
 * `qb::CircuitBreaker`. Smooths bursts to a steady rate; pair with `ask_guarded` / `ask_retry`.
 * @code
 * // ≤ 100 calls/s, bursts up to 10:
 * auto limiter = std::make_shared<qb::rate_limiter>(10.0, std::chrono::milliseconds{10});
 * co_await limiter->acquire(ctx);
 * auto r = co_await qb::ask(ctx, svc, Req{}, 1s);
 * @endcode
 */
class rate_limiter {
public:
    /**
     * @brief Construct a token bucket.
     * @param capacity Burst size — max tokens held (clamped to >= 1).
     * @param per_token Time to regenerate one token (clamped to >= 1 ns).
     */
    rate_limiter(double capacity, qb::duration per_token) noexcept
        : _capacity(capacity < 1.0 ? 1.0 : capacity)
        , _tokens(_capacity)
        , _per_token_ns(per_token.count() > 0 ? static_cast<double>(per_token.count()) : 1.0) {}

    /**
     * @brief Non-blocking: consume a token if one is available at `now_ns`.
     * @param now_ns Current `VirtualCore` timestamp in nanoseconds (e.g. `ctx.time()`).
     * @return `true` and consumes a token, or `false` if the bucket is empty.
     */
    [[nodiscard]] bool
    try_acquire(std::uint64_t now_ns) noexcept {
        refill(now_ns);
        if (_tokens >= 1.0) {
            _tokens -= 1.0;
            return true;
        }
        return false;
    }

    /**
     * @brief Cancellation-aware: wait until a token is available, then consume it.
     * @param ctx The coroutine context (its clock drives refill; its scope cancels the wait).
     * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
     */
    [[nodiscard]] qb::io::async::task<void>
    acquire(qb::ScopedCoroContext ctx) {
        while (!try_acquire(ctx.time())) {
            const double deficit = 1.0 - _tokens; // fractional tokens still needed, in [0, 1)
            const auto   wait_ns = static_cast<qb::duration::rep>(deficit * _per_token_ns);
            co_await ctx.sleep(qb::duration{wait_ns > 0 ? wait_ns : static_cast<qb::duration::rep>(_per_token_ns)});
        }
    }

    /** @brief Current (fractional) token count, after refilling to `now_ns`. */
    [[nodiscard]] double
    tokens(std::uint64_t now_ns) noexcept {
        refill(now_ns);
        return _tokens;
    }

private:
    void
    refill(std::uint64_t now_ns) noexcept {
        if (!_primed) { // first observation primes the clock without granting a windfall
            _last_ns = now_ns;
            _primed  = true;
            return;
        }
        if (now_ns <= _last_ns)
            return;
        _tokens  = (std::min) (_capacity, _tokens + static_cast<double>(now_ns - _last_ns) / _per_token_ns);
        _last_ns = now_ns;
    }

    double        _capacity;
    double        _tokens;
    double        _per_token_ns;
    std::uint64_t _last_ns = 0;
    bool          _primed  = false;
};

/** @brief Alias for `rate_limiter`. */
using token_bucket = rate_limiter;

/**
 * @class bulkhead
 * @ingroup Patterns
 * @brief Bounds the number of **concurrent** operations through a resource (failure isolation).
 * @details
 * A bulkhead caps in-flight calls to a dependency so a slow/failing dependency cannot exhaust the
 * core (the naval "watertight compartment"). `enter(ctx)` acquires a slot, **waiting
 * cancellation-aware** when the pool is full, and returns an RAII `slot` that frees it on scope
 * exit; `try_enter` is the non-blocking variant. Built on the cancel-aware
 * `qb::io::async::semaphore::acquire(token)`, so a killed actor parked on a full bulkhead unwinds
 * cleanly without leaking a slot. The pool is shared by `shared_ptr` (held internally), so a `slot`
 * is self-sufficient even if the `bulkhead` itself is destroyed first. Core-local (single-thread).
 * @code
 * auto bh = std::make_shared<qb::bulkhead>(8); // at most 8 concurrent calls to `svc`
 * spawn([bh, svc](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
 *     auto slot = co_await bh->enter(ctx);      // waits if 8 are already in flight
 *     auto r    = co_await qb::ask(ctx, svc, Req{}, 1s);
 * });                                            // slot frees on scope exit
 * @endcode
 */
class bulkhead {
public:
    /** @brief Create a bulkhead admitting at most `max_concurrent` operations (clamped to >= 1). */
    explicit bulkhead(std::size_t max_concurrent)
        : _sem(std::make_shared<qb::io::async::semaphore>(max_concurrent ? max_concurrent : std::size_t{1})) {}

    /** @brief RAII admission slot — frees its bulkhead permit on destruction (or `release()`). */
    class slot {
        std::shared_ptr<qb::io::async::semaphore> _sem;

    public:
        slot() = default;
        explicit slot(std::shared_ptr<qb::io::async::semaphore> s) noexcept
            : _sem(std::move(s)) {}
        slot(const slot &)            = delete;
        slot &operator=(const slot &) = delete;
        slot(slot &&o) noexcept
            : _sem(std::move(o._sem)) {} // moved-from holds nothing → only the new owner releases
        slot &
        operator=(slot &&o) noexcept {
            if (this != &o) {
                release(); // free our current permit before taking over o's
                _sem = std::move(o._sem);
            }
            return *this;
        }
        ~slot() {
            release();
        }
        /** @brief Free the permit early (idempotent). */
        void
        release() noexcept {
            if (_sem) {
                _sem->release();
                _sem.reset();
            }
        }
    };

    /**
     * @brief Acquire a slot, waiting (cancellation-aware) if the bulkhead is full.
     * @param ctx The coroutine context (its scope cancels the wait on kill).
     * @return A `slot` that frees the permit on scope exit.
     * @throws qb::io::async::cancelled_error if the actor is killed while waiting for a slot.
     */
    [[nodiscard]] qb::io::async::task<slot>
    enter(qb::ScopedCoroContext ctx) {
        co_await _sem->acquire(ctx.token()); // cancel-aware: a kill retracts the claim, no slot leaks
        co_return slot{_sem};
    }

    /**
     * @brief Non-blocking: take a slot if one is free.
     * @param out Receives the slot on success.
     * @return `true` if a slot was admitted, `false` if the bulkhead is full.
     */
    [[nodiscard]] bool
    try_enter(slot &out) {
        if (_sem->try_acquire()) {
            out = slot{_sem};
            return true;
        }
        return false;
    }

    /** @brief Free slots currently available. */
    [[nodiscard]] std::size_t
    available() const noexcept {
        return _sem->available_permits();
    }

private:
    std::shared_ptr<qb::io::async::semaphore> _sem;
};

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
ask_retry(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout, qb::retry_policy policy = {}) {
    const int    max_attempts = policy.max_attempts > 0 ? policy.max_attempts : 1; // doc: >= 1
    qb::duration backoff      = policy.backoff;
    for (int attempt = 1;; ++attempt) {
        try {
            co_return co_await qb::ask<E>(ctx, target, req, timeout);
        } catch (const qb::io::async::timeout_error &) {
            if (attempt >= max_attempts)
                throw; // attempts exhausted — propagate the timeout
        }
        // `co_await` is illegal in a catch handler, so back off here (cancellation-aware).
        // Jitter is applied to the *waited* value only; the geometric series itself stays
        // deterministic so growth is predictable.
        co_await ctx.sleep(qb::detail::apply_retry_jitter(backoff, policy.jitter));
        const auto grown = static_cast<qb::duration::rep>(static_cast<double>(backoff.count()) * policy.multiplier);
        backoff          = (std::min) (qb::duration{grown}, policy.max_backoff);
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
ask_guarded(qb::ScopedCoroContext ctx, std::shared_ptr<qb::CircuitBreaker> breaker, qb::ActorId target, E req, qb::duration timeout) {
    assert(breaker && "qb::ask_guarded requires a non-null CircuitBreaker");
    if (!breaker->allow(ctx.time()))
        throw qb::circuit_open_error{}; // fail fast — the request is never sent

    std::exception_ptr failure;
    try {
        E resp = co_await qb::ask<E>(ctx, target, req, timeout);
        breaker->on_success();
        co_return resp;
    } catch (const qb::io::async::cancelled_error &) {
        // Actor killed — not a breaker failure. Release a half-open trial so the breaker is not
        // wedged half-open (the trial produced no verdict); a future call re-arms after cooldown.
        breaker->on_abandoned(ctx.time());
        throw;
    } catch (...) {
        failure = std::current_exception();
    }
    breaker->on_failure(ctx.time());
    std::rethrow_exception(failure);
}

} // namespace qb

#endif // QB_CORE_PATTERNS_RESILIENCE_H
