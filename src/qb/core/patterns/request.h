/**
 * @file qb/core/patterns/request.h
 * @brief Typed request/response over the actor `ask` primitive.
 *
 * Provides the `qb::Request<Resp>` envelope, the `ask_event_type` concept, and the free
 * functions `qb::ask` (asker side) and `qb::answer` (responder side). These compose only
 * the public kernel primitives (`ScopedCoroContext::push_to/id/token`, `Actor::resolve_ask/
 * reply`) — the kernel holds no request/response logic of its own.
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

#ifndef QB_CORE_PATTERNS_REQUEST_H
#define QB_CORE_PATTERNS_REQUEST_H

#include <concepts>
#include <cstdint>
#include <utility>
#include <qb/core/Actor.h>
#include <qb/io/async/coroutine.h>
#include <qb/system/time.h> // qb::duration

namespace qb {

/**
 * @concept ask_event_type
 * @ingroup Patterns
 * @brief An event usable with `qb::ask` — derives from `qb::AskEvent` (carries `correlation_id`)
 *        and is copyable (the request is copied per attempt by `ask_retry` and per target by
 *        `ask_all`/`ask_any`), so a move-only exchange type is rejected as a clear concept error
 *        rather than a deep template failure.
 */
template <class E>
concept ask_event_type = std::derived_from<E, qb::AskEvent> && std::copyable<E>;

/**
 * @struct Request
 * @ingroup Patterns
 * @brief Typed request/response envelope for `qb::ask` — request fields + a `response` slot.
 * @tparam Resp The response payload type the responder fills in.
 * @details
 * Derive your exchange from `Request<Resp>` and add your **request** fields; the base supplies
 * the `response` slot and the `AskEvent` correlation id, so one event type round-trips the whole
 * exchange.
 * @code
 * struct Quote : qb::Request<double> { qb::string<16> symbol; };// qb::string: events are memcpy-relocated
 *
 * // asker (inside a spawn() coroutine):
 * auto q = co_await qb::ask(ctx, market, Quote{.symbol = "BTC"}, 500ms);  // designated: names the member
 * use(q.response);
 *
 * // responder (synchronous handler):
 * void on(Quote &q) { qb::answer(*this, q, [](Quote const &r){ return lookup(r.symbol); }); }
 * @endcode
 * @see qb::ask, qb::answer, qb::AskEvent
 */
template <class Resp>
struct Request : qb::AskEvent {
    using response_type = Resp; ///< Response payload type — used by generic helpers.
    Resp response{};            ///< Filled by the responder (via `answer`), preserved by `reply()`.
};

/** @brief Alias template for `Request`. */
template <class Resp>
using request = Request<Resp>;

/**
 * @brief Native request/response: send `req` to `target` and `co_await` the reply.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The spawning coroutine's context.
 * @param target The actor to ask.
 * @param req The request event (its response fields are filled by the responder).
 * @param timeout Max time to wait. `<= 0` waits indefinitely (until reply or kill).
 * @return `task<E>` resolving to the response event.
 * @throws qb::io::async::timeout_error if no reply arrives in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Correlation, timeout and cancel-on-kill are handled by a single awaiter (no detached
 *          helper). The responder fills `on(E&)` and `reply()`s it back (preserving the
 *          correlation id); the asker routes responses with `resolve_ask(e)` in its own `on(E&)`.
 * @code
 * auto r = co_await qb::ask(ctx, market, PriceQuery{"BTC"}, 500ms);
 * @endcode
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout) {
    const std::uint64_t aid = qb::detail::ask_next_id(ctx.id());
    req.correlation_id      = aid;
    ctx.template push_to<E>(target, std::move(req)); // send to target, source = asker
    co_return co_await qb::detail::ask_awaiter<E>{aid, ctx.id(), timeout, ctx.token()};
}

/**
 * @struct deadline
 * @ingroup Patterns
 * @brief An **absolute** completion time (UNIX-epoch nanoseconds) shared across an ask chain.
 * @details Propagating one `deadline` down a sequence of `ask_by` calls bounds the *total* time of
 *          the whole chain — unlike a per-`ask` relative `timeout`, which resets at each hop. Build
 *          it with `qb::deadline_in(ctx, dur)`; query the time left with `qb::remaining(dl, ctx)`.
 */
struct deadline {
    std::uint64_t at_ns{0}; ///< absolute deadline, in nanoseconds since the epoch (cf. `Actor::time()`).
};

/** @brief A `deadline` `dur` from now (using the context's cached `VirtualCore` clock).
 *  @note It is an ABSOLUTE instant, so it inherits whatever `ctx.time()` reads at the moment you build it. Through 3.0.0
 *        that was **0 inside `onInit()`** (the field is refreshed by the loop, and `onInit()` runs before the first pass),
 *        which made a deadline built there land in 1970: MEASURED `dl.at_ns = 500000000` for a `500ms` budget, and the
 *        first `remaining()` taken once the loop was running returned **0 ns**, so every `ask_by` on that chain threw
 *        `timeout_error` without sending anything. `VirtualCore.h:377` now seeds the clock at core construction; pinned
 *        by `InitClock.ADeadlineBuiltInOnInitIsNotAlreadyExpired`. */
[[nodiscard]] inline deadline
deadline_in(qb::ScopedCoroContext ctx, qb::duration dur) noexcept {
    const auto add = dur.count() > 0 ? static_cast<std::uint64_t>(dur.count()) : std::uint64_t{0};
    return deadline{ctx.time() + add};
}

/** @brief Time left until `dl` (clamped to zero — never negative). */
[[nodiscard]] inline qb::duration
remaining(deadline dl, qb::ScopedCoroContext ctx) noexcept {
    const auto now = ctx.time();
    return dl.at_ns > now ? qb::duration{static_cast<qb::duration::rep>(dl.at_ns - now)} : qb::duration::zero();
}

/**
 * @brief `ask` bounded by an absolute `deadline` (shared budget) instead of a relative timeout.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param target The actor to ask.
 * @param req The request.
 * @param dl The shared deadline; the underlying `ask` uses `remaining(dl, ctx)` as its timeout.
 * @return `task<E>` resolving to the response.
 * @throws qb::io::async::timeout_error immediately if the budget is already spent, or if the reply
 *         does not arrive before `dl`.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Thread the **same** `deadline` through every hop of a request chain to bound its total
 *          latency end-to-end:
 * @code
 * auto dl = qb::deadline_in(ctx, 1s);              // whole chain must finish within 1 s
 * auto a  = co_await qb::ask_by(ctx, svc1, R1{}, dl);
 * auto b  = co_await qb::ask_by(ctx, svc2, R2{a.response}, dl); // gets only the time svc1 left
 * @endcode
 * @see qb::ask, qb::deadline, qb::remaining
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_by(qb::ScopedCoroContext ctx, qb::ActorId target, E req, deadline dl) {
    const qb::duration left = remaining(dl, ctx);
    if (left <= qb::duration::zero())
        throw qb::io::async::timeout_error{}; // budget already spent — fail fast, send nothing
    co_return co_await qb::ask<E>(ctx, target, std::move(req), left);
}

/**
 * @brief Responder helper for the typed `Request`/`ask` pattern — fill the response and reply.
 * @ingroup Patterns
 * @tparam E A `qb::Request<Resp>` subtype (carries `request` fields + a `response` slot).
 * @tparam Fn Callable `Resp(E const&)` computing the response from the request.
 * @param self The responding actor.
 * @param e The received request event.
 * @param fn Computes the response payload from `e` (runs synchronously, full actor access).
 * @details
 * Call this from the responder's `on(E&)`. It first routes any reply to one of `self`'s own
 * pending asks via `resolve_ask(e)` (returning early if so), then computes `e.response = fn(e)`
 * and `reply()`s the same event back to the asker (preserving the correlation id).
 * @code
 * void on(Quote &q) { qb::answer(*this, q, [](Quote const &r){ return lookup(r.symbol); }); }
 * @endcode
 * @warning `fn` runs synchronously inside the responder's message handler and **must not throw**.
 *          If it throws, `answer` (which is `noexcept` iff `fn` is) propagates the exception out of
 *          `on(E&)` into the event dispatch, which — like any throwing actor handler — terminates
 *          the worker core (there is no per-event exception containment on the steady-state dispatch
 *          path). `reply()` is also skipped, so the asker would only ever observe its `ask` timeout.
 *          Compute the response with a non-throwing `fn` (validate/look up before `answer`, or carry
 *          a failure indicator in the response payload and reply it explicitly).
 */
template <class E, class Fn>
void
answer(qb::Actor &self, E &e, Fn &&fn) noexcept(noexcept(std::forward<Fn>(fn)(e))) {
    if (self.resolve_ask(e))
        return; // it was a reply to one of our own asks — already delivered to the coroutine.
    e.response = std::forward<Fn>(fn)(e);
    self.reply(e);
}

} // namespace qb

#endif // QB_CORE_PATTERNS_REQUEST_H
