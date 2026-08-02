/**
 * @file qb/core/patterns/discovery.h
 * @brief Coroutine actor discovery & liveness: `qb::ping` and `qb::require`.
 *
 * Modern, awaitable replacement for the legacy fire-and-forget `Actor::require<...>()` +
 * `on(RequireEvent&)` + `is<T>()` dance. Built on the kernel `PingEvent`/`RequireEvent` (which now
 * carry an echoed `correlation_id`):
 *   - `co_await qb::ping(ctx, target, timeout)` → `bool` — targeted liveness probe.
 *   - `co_await qb::require<T>(ctx, timeout)` → `std::vector<ActorId>` — discover live actors of a
 *     type within a time window.
 * Replies are routed automatically by `Actor`'s default `on(RequireEvent&)` (which calls
 * `resolve_require`) — no boilerplate; override `on(RequireEvent&)` only for the legacy `is<T>()`
 * dance. Core-local (single thread); cancel-on-kill.
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

#ifndef QB_CORE_PATTERNS_DISCOVERY_H
#define QB_CORE_PATTERNS_DISCOVERY_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/io/async/coroutine.h> // cancellation_token / schedule_via_current

namespace qb {

namespace detail {

/**
 * @brief Shared state for one in-flight discovery (ping = single reply, require = collect window).
 * @details Registers an `ask_slot` in the per-core continuation registry (the same one ask uses), so
 *          replies are delivered uniformly whether the actor is active OR still *Activating* — the
 *          activation gate routes correlated replies through that registry. Multi-shot: its deliver
 *          thunk never sets `slot.done`, so successive `RequireEvent` replies keep arriving until the
 *          discovery deregisters.
 */
struct discovery_state {
    std::vector<std::pair<std::uint32_t, qb::ActorId>> found;          ///< (type, responder) replies
    bool                                               single = false; ///< ping stops at the first reply
    bool                                               done   = false;
    qb::io::async::cancellation_token                  token; ///< actor scope (cancel-on-kill)
    std::coroutine_handle<>                            waiter{};
    qb::detail::ask_slot                               slot{}; ///< entry in the continuation registry

    void
    wake() noexcept {
        if (waiter)
            qb::io::async::schedule_via_current(std::exchange(waiter, std::coroutine_handle<>{}));
    }
    void
    deliver(std::uint32_t type, qb::ActorId src) {
        if (done)
            return;
        found.emplace_back(type, src);
        if (single) { // ping: first reply resolves it; require: collect until the window elapses
            done = true;
            wake();
        }
    }
    static void
    deliver_thunk(void *self, qb::Event &ev) noexcept {
        auto *st = static_cast<discovery_state *>(self);
        auto &re = static_cast<qb::RequireEvent &>(ev); // type registered ⇒ derives CorrelatedEvent
        st->deliver(re.type, re.getSource());
    }
};

/** @brief Parks the ping/require coroutine until a resolving reply, the time window, or a kill. */
struct discovery_awaiter {
    std::shared_ptr<discovery_state>           st;
    qb::duration                               timeout;
    std::uint64_t                              id;
    ev_timer                                   timer{};
    bool                                       timer_started = false;
    std::shared_ptr<bool>                      alive         = std::make_shared<bool>(true);
    qb::io::async::cancellation_token::id_type cancel_id     = 0;

    discovery_awaiter(std::shared_ptr<discovery_state> s, qb::duration t, std::uint64_t i)
        : st(std::move(s))
        , timeout(t)
        , id(i) {}
    discovery_awaiter(const discovery_awaiter &)            = delete;
    discovery_awaiter &operator=(const discovery_awaiter &) = delete;
    ~discovery_awaiter() {
        if (alive)
            *alive = false;
        stop_timer();
        st->token.remove_on_cancel(cancel_id);
        qb::detail::ask_unregister(id); // idempotent; covers a destroy-without-resume (matches ask_awaiter)
    }

    [[nodiscard]] bool
    await_ready() const noexcept {
        return st->token.is_cancelled() || st->done;
    }
    void
    await_suspend(std::coroutine_handle<> h) {
        st->waiter = h;
        if (timeout.count() > 0) {
            ev_timer_init(&timer, &discovery_awaiter::on_timeout, qb::detail::to_ev_seconds(timeout), 0.0);
            timer.data = this;
            auto loop  = qb::detail::ask_loop();
            ev_now_update(static_cast<struct ev_loop *>(loop));
            ev_timer_start(loop, &timer);
            timer_started = true;
        }
        auto a    = alive;
        cancel_id = st->token.on_cancel([this, a]() {
            if (*a)
                st->wake();
        });
    }
    void
    await_resume() {
        stop_timer();
        st->token.remove_on_cancel(cancel_id);
        cancel_id  = 0;
        st->waiter = {};
        qb::detail::ask_unregister(id); // leave the continuation registry
        if (st->token.is_cancelled())
            throw qb::io::async::cancelled_error{}; // killed mid-discovery
        // else: st->found holds the responders (empty on timeout)
    }

private:
    void
    stop_timer() noexcept {
        if (timer_started) {
            ev_timer_stop(qb::detail::ask_loop(), &timer);
            timer_started = false;
        }
    }
    static void
    on_timeout(struct ev_loop *, ev_timer *w, int) noexcept {
        auto *me = static_cast<discovery_awaiter *>(w->data);
        if (me)
            me->st->wake(); // window elapsed (require) / no reply (ping)
    }
};

/** @brief Register a discovery's continuation slot in the per-core registry. */
inline void
register_discovery(std::uint64_t id, discovery_state &st, qb::ActorId owner) noexcept {
    st.slot.owner   = owner;
    st.slot.done    = false;
    st.slot.self    = &st;
    st.slot.deliver = &discovery_state::deliver_thunk;
    qb::detail::ask_register(id, &st.slot);
    // Make `RequireEvent` recognisable to the activation gate so replies reach an Activating asker.
    qb::detail::ask_register_type(qb::Event::template type_to_id<qb::RequireEvent>());
}

/**
 * @brief Send the wildcard liveness `PingEvent` for `ping()`.
 * @details Deliberately not inline in `ping()`. GCC does not emit a function template whose FIRST
 *          point of instantiation is inside a coroutine body, so calling
 *          `ctx.push_to<qb::PingEvent>(...)` directly from `ping()` (a `qb::io::async::task<bool>`)
 *          links clean under clang and fails under GCC with
 *          `undefined reference to qb::CoroContext::push_to<qb::PingEvent, ...>`. Instantiating
 *          from an ordinary function moves that point out of the coroutine.
 */
inline void
send_ping(qb::ScopedCoroContext &ctx, qb::ActorId target, std::uint64_t id) {
    ctx.template push_to<qb::PingEvent>(target, std::uint32_t{0}, id);
}

} // namespace detail

/**
 * @brief Targeted liveness probe: is `target` alive and responsive within `timeout`?
 * @ingroup Patterns
 * @param ctx The spawning coroutine's context (its scope cancels the wait on kill).
 * @param target The actor to probe.
 * @param timeout Max time to wait for the reply (defaults to 1 s).
 * @return `task<bool>` — `true` if `target` replied in time, `false` otherwise.
 * @throws qb::io::async::cancelled_error if the probing actor is killed while waiting.
 * @details Sends a wildcard `PingEvent` (any live actor replies regardless of type). Works inside
 *          `onInit()` too — the reply is delivered through the continuation registry even while the
 *          asker is Activating. No `on(RequireEvent&)` handler needed (Actor routes it by default).
 */
[[nodiscard]] inline qb::io::async::task<bool>
ping(qb::ScopedCoroContext ctx, qb::ActorId target, qb::duration timeout = std::chrono::seconds{1}) {
    auto st       = std::make_shared<detail::discovery_state>();
    st->single    = true;
    st->token     = ctx.token();
    const auto id = qb::detail::ask_next_id(ctx.id());
    detail::register_discovery(id, *st, ctx.id());
    qb::detail::ask_slot_guard guard{id}; // deregister if the send throws before the awaiter takes over
    detail::send_ping(ctx, target, id);   // type 0 = wildcard liveness; see send_ping for why it is not inline here
    guard.release();                      // the awaiter (its dtor unregisters) owns the slot from here
    co_await detail::discovery_awaiter{st, timeout, id};
    co_return !st->found.empty();
}

/**
 * @brief Discover all live actors of type `_Actor` reachable within `timeout`.
 * @ingroup Patterns
 * @tparam _Actor The actor type to discover.
 * @param ctx The spawning coroutine's context.
 * @param timeout Collection window (defaults to 200 ms) — replies arriving within it are gathered.
 * @return `task<std::vector<qb::ActorId>>` — the responders' ids (empty if none).
 * @throws qb::io::async::cancelled_error if the requesting actor is killed while waiting.
 * @details Broadcasts a typed `PingEvent` to every core and collects the `RequireEvent` replies for
 *          the whole window. Awaitable replacement for the legacy `require<_Actor>()` + the
 *          `on(RequireEvent&)` / `is<_Actor>()` dance. Works inside `onInit()` (discover-before-
 *          activate) — replies reach the Activating asker through the continuation registry. No
 *          `on(RequireEvent&)` handler needed (Actor routes it by default).
 * @code
 * auto workers = co_await qb::require<Worker>(ctx, 200ms);
 * for (auto id : workers) ctx.push_to<Job>(id, ...);
 * @endcode
 */
template <typename _Actor>
[[nodiscard]] qb::io::async::task<std::vector<qb::ActorId>>
require(qb::ScopedCoroContext ctx, qb::duration timeout = std::chrono::milliseconds{200}) {
    auto st       = std::make_shared<detail::discovery_state>();
    st->single    = false;
    st->token     = ctx.token();
    const auto id = qb::detail::ask_next_id(ctx.id());
    detail::register_discovery(id, *st, ctx.id());
    qb::detail::ask_slot_guard guard{id}; // deregister if the broadcast throws before the awaiter takes over
    ctx.template broadcast<qb::PingEvent>(static_cast<std::uint32_t>(qb::type_id<_Actor>()), id);
    guard.release(); // the awaiter (its dtor unregisters) owns the slot from here
    co_await detail::discovery_awaiter{st, timeout, id};
    std::vector<qb::ActorId> out;
    out.reserve(st->found.size());
    for (auto const &[type, src] : st->found) {
        (void) type; // single type broadcast → every reply matches _Actor
        out.push_back(src);
    }
    co_return out;
}

} // namespace qb

#endif // QB_CORE_PATTERNS_DISCOVERY_H
