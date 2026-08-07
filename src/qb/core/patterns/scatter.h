/**
 * @file qb/core/patterns/scatter.h
 * @brief Scatter-gather over `qb::ask` — `ask_all` (await every reply) and `ask_any` (first wins).
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

#ifndef QB_CORE_PATTERNS_SCATTER_H
#define QB_CORE_PATTERNS_SCATTER_H

#include <algorithm>
#include <any>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <utility>
#include <vector>
#include <qb/io/async/coroutine.h>
#include "request.h"

namespace qb {

/**
 * @brief Scatter-gather: `ask` every target with a copy of `req`, await **all** replies.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask (each receives its own copy + correlation id).
 * @param req The request, copied per target.
 * @param timeout Per-ask timeout.
 * @return `task<std::vector<E>>` — one filled response envelope per target, in input order.
 * @throws qb::io::async::timeout_error if **any** target fails to reply in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Built on `when_all` over per-target `qb::ask`s — replicated reads, quorum inputs,
 *          fan-out queries. A cross-core scatter keeps N asks in flight at once (bounded by
 *          `timeout`).
 * @code
 * auto quotes = co_await qb::ask_all(ctx, markets, Quote{.symbol = "BTC"}, 500ms);
 * for (auto const &q : quotes) use(q.response);
 * @endcode
 * @see qb::ask, qb::ask_any
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<std::vector<E>>
ask_all(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, E req, qb::duration timeout) {
    std::vector<qb::io::async::task<E>> calls;
    calls.reserve(targets.size());
    for (auto const target : targets)
        calls.emplace_back(qb::ask<E>(ctx, target, req, timeout));
    co_return co_await qb::io::async::when_all(std::move(calls));
}

namespace detail {
// One target ask gated by a shared semaphore: acquire a slot (cancellation-aware), ask, then
// release the slot — so concurrency across the whole scatter never exceeds the semaphore's permits
// (a true sliding window: a new ask starts the instant one finishes, no wave barrier).
template <typename E>
qb::io::async::task<E>
gated_ask(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout, std::shared_ptr<qb::io::async::semaphore> sem) {
    co_await sem->acquire(ctx.token()); // cancel-aware: a kill while parked retracts the claim
    struct release_guard {              // free the slot on completion OR on a thrown ask
        std::shared_ptr<qb::io::async::semaphore> s;
        ~release_guard() {
            if (s)
                s->release();
        }
    } guard{sem};
    co_return co_await qb::ask<E>(ctx, target, std::move(req), timeout);
}
} // namespace detail

/**
 * @brief Bounded scatter-gather: like `ask_all`, but at most `max_in_flight` asks run at once.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask (each receives its own copy + correlation id).
 * @param req The request, copied per target.
 * @param timeout Per-ask timeout.
 * @param max_in_flight Concurrency cap (`0` ⇒ unbounded, i.e. equivalent to plain `ask_all`).
 * @return `task<std::vector<E>>` — one filled response envelope per target, in input order.
 * @throws qb::io::async::timeout_error if any target fails to reply in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details A true **sliding window**: all asks are launched at once but each waits on a shared,
 *          cancellation-aware `qb::io::async::semaphore` of `max_in_flight` permits, so a new ask
 *          starts the instant an earlier one finishes (no wave barriers) while concurrency never
 *          exceeds the cap. A killed actor unwinds cleanly — both the gate and the ask are
 *          cancel-on-kill, and a parked gate claim is retracted (no permit leak). Use it to fan out
 *          to many targets without overwhelming a shared downstream.
 * @code
 * auto results = co_await qb::ask_all(ctx, hundred_targets, Probe{}, 200ms, 8); // ≤ 8 at a time
 * @endcode
 * @see qb::ask_all, qb::ask_any, qb::bulkhead
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<std::vector<E>>
ask_all(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, E req, qb::duration timeout, std::size_t max_in_flight) {
    if (max_in_flight == 0 || max_in_flight >= targets.size())
        co_return co_await ask_all(ctx, std::move(targets), std::move(req), timeout); // unbounded
    auto                                sem = std::make_shared<qb::io::async::semaphore>(max_in_flight);
    std::vector<qb::io::async::task<E>> calls;
    calls.reserve(targets.size());
    for (auto const target : targets)
        calls.emplace_back(detail::gated_ask<E>(ctx, target, req, timeout, sem));
    co_return co_await qb::io::async::when_all(std::move(calls));
}

/**
 * @brief Race: `ask` every target, resolve with the **first** reply; the others are abandoned.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask.
 * @param req The request, copied per target.
 * @param timeout Shared timeout — if no reply arrives within it, throws `timeout_error`.
 * @return `task<E>` — the first responder's filled envelope.
 * @throws qb::io::async::timeout_error if no target replies in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Built on `when_any`. The losing asks are **reclaimed** the instant a winner replies —
 *          `when_any` tears each loser's coroutine down (stopping its ask timer) rather than letting
 *          it linger until its own `timeout`. Use it for "fastest replica wins" / hedged requests.
 * @see qb::ask, qb::ask_all
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_any(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, E req, qb::duration timeout) {
    std::vector<qb::io::async::task<E>> calls;
    calls.reserve(targets.size());
    for (auto const target : targets)
        calls.emplace_back(qb::ask<E>(ctx, target, req, timeout));
    auto winner = co_await qb::io::async::when_any(std::move(calls));
    co_return std::any_cast<E>(std::move(winner.second));
}

namespace detail {

// Shared state for ask_quorum, mutated only on the owning VirtualCore thread (mono-thread
// cooperative — no atomics needed, exactly like cancellable_operation's shared_state). Held by
// shared_ptr in every collector frame AND the awaiter, so it outlives all of them.
template <typename E>
struct quorum_state {
    std::vector<E>          results;       ///< the first `need` successful replies (completion order)
    std::size_t             need  = 0;     ///< K
    std::size_t             total = 0;     ///< N
    std::size_t             ok    = 0;     ///< successes so far
    std::size_t             fail  = 0;     ///< failures so far
    bool                    done  = false; ///< quorum reached OR provably unreachable
    std::coroutine_handle<> cont{};        ///< the parked ask_quorum coroutine (null until suspended)
    std::exception_ptr      error;         ///< first non-cancel failure (propagated if unreachable)
};

// Wake the parked ask_quorum coroutine, exactly once. Schedules (never resumes inline) to avoid
// re-entering the scheduler from within a collector's resume.
template <typename E>
inline void
quorum_wake(quorum_state<E> &st) noexcept {
    if (st.cont)
        qb::io::async::schedule_via_current(std::exchange(st.cont, {}));
}

// Parks the ask_quorum coroutine until the shared state is `done` (set by a collector).
template <typename E>
struct quorum_awaiter {
    std::shared_ptr<quorum_state<E>>  st;
    qb::io::async::cancellation_token token;    // the actor scope — a kill takes priority on resume
    std::coroutine_handle<>           parked{}; ///< handle stored in st->cont (cleared on teardown)

    // User-declared dtor below makes this a non-aggregate → provide the ctor ask_quorum uses.
    quorum_awaiter(std::shared_ptr<quorum_state<E>> s, qb::io::async::cancellation_token t)
        : st(std::move(s))
        , token(std::move(t)) {}

    // Destroyed while still parked (await_resume never ran — e.g. a when_any/race loser reclaim):
    // the DETACHED collectors still hold `st` and would later quorum_wake() → schedule_via_current()
    // our freed frame. Clear st->cont so that wake is a no-op. The held `st` keeps the state valid.
    ~quorum_awaiter() {
        if (parked && st->cont == parked)
            st->cont = {};
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return st->done || token.is_cancelled();
    }
    void
    await_suspend(std::coroutine_handle<> h) noexcept {
        st->cont = h; // collectors are already spawned; they will wake us (or already have)
        parked   = h;
    }
    std::vector<E>
    await_resume() {
        if (token.is_cancelled())
            throw qb::io::async::cancelled_error{}; // killed — propagate as cancellation
        if (st->ok < st->need) {                    // quorum proved unreachable
            if (st->error)
                std::rethrow_exception(st->error); // surface the first underlying failure
            throw qb::io::async::timeout_error{};  // unreachable with no captured error
        }
        return std::move(st->results);
    }
};

} // namespace detail

/**
 * @brief Quorum scatter: `ask` every target, resolve with the **first `k`** successful replies.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask (each receives its own copy + correlation id).
 * @param k Number of successful replies required (clamped to `[1, targets.size()]`).
 * @param req The request, copied per target.
 * @param timeout Per-ask timeout.
 * @return `task<std::vector<E>>` — exactly `k` filled response envelopes, in completion order
 *         (fastest first). An empty vector if `k == 0` or there are no targets.
 * @throws qb::io::async::timeout_error if the quorum becomes **unreachable** (too many targets
 *         failed/timed out for `k` successes to be possible) — carries the first underlying error.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Fills the gap between `ask_any` (k = 1) and `ask_all` (k = N): replicated reads /
 *          quorum writes that need a majority. Surplus replies beyond `k` are dropped (their asks
 *          linger to their own `timeout`, like `ask_any`'s losers). Every per-target ask is
 *          cancel-on-kill, so a killed actor unwinds cleanly.
 * @code
 * auto majority = co_await qb::ask_quorum(ctx, replicas, replicas.size()/2 + 1, Read{key}, 200ms);
 * @endcode
 * @see qb::ask_any, qb::ask_all
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<std::vector<E>>
ask_quorum(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, std::size_t k, E req, qb::duration timeout) {
    const std::size_t n = targets.size();
    if (k == 0 || n == 0)
        co_return std::vector<E>{};
    if (k > n)
        k = n;
    auto st   = std::make_shared<detail::quorum_state<E>>();
    st->need  = k;
    st->total = n;
    st->results.reserve(k);
    // One collector per target (lambda-safe spawn — closure owned, no trailing `()`): await the
    // scope-bound ask, fold the outcome into the shared state, wake the parked quorum when decided.
    for (auto const target : targets) {
        qb::io::async::coro_scheduler().spawn([ctx, target, req, timeout, st]() -> qb::io::async::task<void> {
            try {
                E r = co_await qb::ask<E>(ctx, target, req, timeout);
                if (!st->done) {
                    st->results.emplace_back(std::move(r));
                    if (++st->ok >= st->need) {
                        st->done = true;
                        detail::quorum_wake(*st);
                    }
                }
            } catch (...) {
                if (!st->done) {
                    // First real error (not a kill) — surfaced if the quorum proves unreachable.
                    if (!st->error)
                        st->error = std::current_exception();
                    // Unreachable once even all remaining successes could not reach `need`.
                    if (++st->fail > st->total - st->need) {
                        st->done = true;
                        detail::quorum_wake(*st);
                    }
                }
            }
        });
    }
    co_return co_await detail::quorum_awaiter<E>{st, ctx.token()};
}

} // namespace qb

#endif // QB_CORE_PATTERNS_SCATTER_H
