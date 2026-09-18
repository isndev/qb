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

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <optional>
#include <tuple>
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
 * @class ask_operation
 * @ingroup Patterns
 * @brief What `qb::ask` returns: the exchange as an awaitable that lives in the CALLER's coroutine
 *        frame — no coroutine frame of its own (Huly QB-214).
 * @tparam E The exchange event type (an `ask_event_type`).
 * @details `co_await qb::ask(...)` used to enter a `task<E>` coroutine whose whole body was "build
 *          the awaiter, stamp, push, `co_await` the awaiter": every ask paid a pooled frame (allocate
 *          and free), the initial suspend and the symmetric transfer into that frame, a `co_return`
 *          moving the 64-byte reply into the promise's `variant`, the final suspend and the transfer
 *          back, and a second move of the reply out of the `variant`. This object IS the exchange —
 *          the context, the target, the request and the timeout, plus the `ask_awaiter` engaged in
 *          place by `await_suspend()` — and a prvalue of it is built straight into the awaiting
 *          frame. Nothing is sent until it is `co_await`ed (the laziness of `task` is kept):
 *          `await_ready()` is the scope's cancel flag, so a cancelled scope sends nothing and
 *          `await_resume()` throws `cancelled_error` at once; `await_suspend()` takes the registry
 *          entry, stamps `correlation_id`, pushes the request and arms the deadline and the cancel
 *          hook; `await_resume()` hands the reply over with a single move. It converts implicitly to
 *          `task<E>` (rvalues only) for the shapes that need a task — `task<E> t = qb::ask(...)`,
 *          `calls.emplace_back(qb::ask(...))` behind `when_all(std::vector<task<E>>)` — and the
 *          variadic `when_all` / `when_any` / `race` and `coro_with_timeout` take it directly.
 *          Move-constructible until it is awaited (the registry holds the engaged awaiter by
 *          address), never copyable.
 * @see qb::ask, qb::ask_emplace_operation, qb::detail::ask_awaiter
 */
template <ask_event_type E>
class ask_operation {
    qb::ScopedCoroContext                     _ctx;
    qb::ActorId                               _target;
    qb::duration                              _timeout;
    E                                         _req;
    std::optional<qb::detail::ask_awaiter<E>> _aw; ///< engaged by `await_suspend()`; address-stable from there

public:
    using value_type = E;                      ///< what `co_await` yields
    using task_type  = qb::io::async::task<E>; ///< the task this converts to

    ask_operation(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout)
        : _ctx(std::move(ctx))
        , _target(target)
        , _timeout(timeout)
        , _req(std::move(req)) {}
    ask_operation(const ask_operation &)            = delete;
    ask_operation &operator=(const ask_operation &) = delete;
    ask_operation &operator=(ask_operation &&)      = delete;
    /// Movable only while not yet awaited: the registry holds the engaged awaiter by address.
    ask_operation(ask_operation &&o) noexcept
        : _ctx(std::move(o._ctx))
        , _target(o._target)
        , _timeout(o._timeout)
        , _req(std::move(o._req)) {
        assert(!o._aw && "qb::ask: an ask_operation cannot be moved once awaited");
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _ctx.token().is_cancelled(); // a cancelled scope sends nothing: await_resume() throws
    }
    void
    await_suspend(std::coroutine_handle<> h) {
        auto &aw            = _aw.emplace(_ctx.id(), _timeout, _ctx.token()); // takes (and binds) the registry entry
        _req.correlation_id = aw.id;
        _ctx.template push_to<E>(_target, std::move(_req)); // send to target, source = asker
        aw.await_suspend(h);                                // deadline + scope cancel hook (or resume at once if cancelled)
    }
    E
    await_resume() {
        if (!_aw)
            throw qb::io::async::cancelled_error(); // await_ready() was true: nothing was sent
        return _aw->await_resume();
    }
    /// The `task<E>` form, for a shape that needs a task: the same exchange inside one pooled frame.
    operator task_type() &&;
};

/**
 * @class ask_emplace_operation
 * @ingroup Patterns
 * @brief What the emplace `qb::ask<E>(ctx, target, timeout, args...)` returns: `ask_operation` with
 *        the constructor arguments held instead of a built request — the event is constructed in
 *        the pipe slot by `await_suspend()`, nothing is copied at all (Huly QB-214).
 * @tparam E The exchange event type (an `ask_event_type`).
 * @tparam Args The constructor arguments, held by value (moved in, moved out into the slot).
 * @see qb::ask, qb::ask_operation
 */
template <ask_event_type E, typename... Args>
class ask_emplace_operation {
    qb::ScopedCoroContext                     _ctx;
    qb::ActorId                               _target;
    qb::duration                              _timeout;
    std::tuple<Args...>                       _args;
    std::optional<qb::detail::ask_awaiter<E>> _aw; ///< engaged by `await_suspend()`; address-stable from there

public:
    using value_type = E;                      ///< what `co_await` yields
    using task_type  = qb::io::async::task<E>; ///< the task this converts to

    ask_emplace_operation(qb::ScopedCoroContext ctx, qb::ActorId target, qb::duration timeout, Args... args)
        : _ctx(std::move(ctx))
        , _target(target)
        , _timeout(timeout)
        , _args(std::move(args)...) {}
    ask_emplace_operation(const ask_emplace_operation &)            = delete;
    ask_emplace_operation &operator=(const ask_emplace_operation &) = delete;
    ask_emplace_operation &operator=(ask_emplace_operation &&)      = delete;
    /// Movable only while not yet awaited: the registry holds the engaged awaiter by address.
    ask_emplace_operation(ask_emplace_operation &&o) noexcept
        : _ctx(std::move(o._ctx))
        , _target(o._target)
        , _timeout(o._timeout)
        , _args(std::move(o._args)) {
        assert(!o._aw && "qb::ask: an ask_emplace_operation cannot be moved once awaited");
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _ctx.token().is_cancelled(); // a cancelled scope sends nothing: await_resume() throws
    }
    void
    await_suspend(std::coroutine_handle<> h) {
        auto &aw           = _aw.emplace(_ctx.id(), _timeout, _ctx.token()); // takes (and binds) the registry entry
        E    &req          = std::apply([this](Args &...a) -> E             &{ return _ctx.template push_to<E>(_target, std::move(a)...); },
                                        _args); // built in the pipe slot
        req.correlation_id = aw.id;
        aw.await_suspend(h); // deadline + scope cancel hook (or resume at once if cancelled)
    }
    E
    await_resume() {
        if (!_aw)
            throw qb::io::async::cancelled_error(); // await_ready() was true: nothing was sent
        return _aw->await_resume();
    }
    /// The `task<E>` form, for a shape that needs a task: the same exchange inside one pooled frame.
    operator task_type() &&;
};

namespace detail {
/// The `task<E>` behind `ask_operation`'s conversion: one pooled frame around the frame-free exchange.
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_task(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout) {
    qb::io::async::pin_frame_copy(req); // QB-213: the copy stays at its own alignment on clang < 22
    co_return co_await qb::ask_operation<E>(std::move(ctx), target, std::move(req), timeout);
}
/// The `task<E>` behind `ask_emplace_operation`'s conversion.
template <ask_event_type E, typename... Args>
[[nodiscard]] qb::io::async::task<E>
ask_emplace_task(qb::ScopedCoroContext ctx, qb::ActorId target, qb::duration timeout, Args... args) {
    (qb::io::async::pin_frame_copy(args), ...); // QB-213: an over-aligned argument stays at its own alignment
    co_return co_await qb::ask_emplace_operation<E, Args...>(std::move(ctx), target, timeout, std::move(args)...);
}
} // namespace detail

template <ask_event_type E>
ask_operation<E>::
operator qb::io::async::task<E>() && {
    return detail::ask_task<E>(std::move(_ctx), _target, std::move(_req), _timeout);
}
template <ask_event_type E, typename... Args>
ask_emplace_operation<E, Args...>::
operator qb::io::async::task<E>() && {
    return std::apply([this](Args &...a) { return detail::ask_emplace_task<E, Args...>(std::move(_ctx), _target, _timeout, std::move(a)...); },
                      _args);
}

/**
 * @brief Native request/response: send `req` to `target` and `co_await` the reply.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The spawning coroutine's context.
 * @param target The actor to ask.
 * @param req The request event (its response fields are filled by the responder).
 * @param timeout Max time to wait. `<= 0` waits indefinitely (until reply or kill).
 * @return An `ask_operation<E>`: `co_await` it for the response event — it lives in your frame, no
 *         coroutine frame of its own — or let it convert to a `task<E>` where a task is needed.
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
[[nodiscard]] ask_operation<E>
ask(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout) {
    return ask_operation<E>(std::move(ctx), target, std::move(req), timeout); // a prvalue: built in the caller's frame
}

/**
 * @brief `ask`, **emplace** form: the request is constructed from `args` directly in the
 *        outgoing pipe slot, instead of being built by the caller and copied in.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`). Explicit — it cannot be deduced,
 *           which is also what keeps this overload out of every existing `qb::ask(ctx, target,
 *           E{...}, timeout)` call.
 * @tparam Args Constructor arguments of `E`, taken BY VALUE and moved into the event: the
 *              operation is lazy, so a reference parameter here would name the caller's
 *              temporaries at a moment they may already be gone.
 * @param ctx The coroutine context.
 * @param target The actor to ask.
 * @param timeout Max time to wait. `<= 0` waits indefinitely (until reply or kill).
 * @param args Forwarded to `E`'s constructor.
 * @return An `ask_emplace_operation<E, Args...>` — identical contract to the by-value form.
 * @details Every `qb::Event` is cache-line aligned, so the by-value form moves a ≥ 64-byte
 *          object twice before it reaches the pipe — the caller's temporary into the operation,
 *          the operation into the pipe slot — and the FIRST of those copies reads back, with
 *          16-byte loads, header fields the constructor has just written with narrow stores: a
 *          store-forwarding stall on every ask, on top of the copies. This form writes each
 *          field exactly once, in place, and touches no temporary at all. Prefer it whenever
 *          the request is built from a handful of values; keep the by-value form for a request
 *          you already hold (retry loops, fan-out).
 * @code
 * auto r = co_await qb::ask<Deposit>(ctx, account, 500ms, amount, txn);
 * @endcode
 */
template <ask_event_type E, typename... Args>
[[nodiscard]] ask_emplace_operation<E, Args...>
ask(qb::ScopedCoroContext ctx, qb::ActorId target, qb::duration timeout, Args... args) {
    return ask_emplace_operation<E, Args...>(std::move(ctx), target, timeout, std::move(args)...); // a prvalue: built in the caller's frame
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
 * auto c  = co_await qb::ask_by<R3>(ctx, svc3, dl, b.response);  // emplace form, same budget
 * @endcode
 * @see qb::ask, qb::deadline, qb::remaining
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_by(qb::ScopedCoroContext ctx, qb::ActorId target, E req, deadline dl) {
    qb::io::async::pin_frame_copy(req); // QB-213: the copy stays at its own alignment on clang < 22
    const qb::duration left = remaining(dl, ctx);
    if (left <= qb::duration::zero())
        throw qb::io::async::timeout_error{}; // budget already spent — fail fast, send nothing
    co_return co_await qb::ask<E>(ctx, target, std::move(req), left);
}

/**
 * @brief `ask_by`, **emplace** form — the request is built in the pipe slot from `args`
 *        (see the emplace `qb::ask`); same deadline contract as the by-value form.
 * @ingroup Patterns
 */
template <ask_event_type E, typename... Args>
[[nodiscard]] qb::io::async::task<E>
ask_by(qb::ScopedCoroContext ctx, qb::ActorId target, deadline dl, Args... args) {
    const qb::duration left = remaining(dl, ctx);
    if (left <= qb::duration::zero())
        throw qb::io::async::timeout_error{}; // budget already spent — fail fast, send nothing
    co_return co_await qb::ask<E>(ctx, target, left, std::move(args)...);
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
