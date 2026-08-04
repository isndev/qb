/**
 * @file qb/core/Actor.tpp
 * @brief Template implementation for the Actor class
 *
 * This file contains the template implementation of the Actor class methods defined
 * in Actor.h. It provides the actual implementation of event handling, actor creation,
 * and inter-actor communication mechanisms.
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
 * @ingroup Core
 */

#include "VirtualCore.h" // also carries VirtualCore's own template bodies since 3.0

#include <cassert>

// ============================================================================================
// HAZARD -- READ BEFORE ADDING ANYTHING BELOW THIS LINE.
//
// TEMPLATES ONLY. Every one of the 41 definitions in this file is a template, and that is the
// only thing keeping the program linkable.
//
// This file is included by TWO disjoint populations:
//   * Actor.cpp:29                          -> compiled once, into libqb-core.a
//   * qb/actor.h, qb/main.h, qb/patterns.h,
//     qb/core/patterns.h, and (since 3.0)
//     core/patterns/{discovery,supervisor}.h -> compiled in EVERY consumer TU
// The QB_ACTOR_TPL guard below stops the double inclusion *within one TU*. It does nothing
// across TUs, and nothing at all between a consumer TU and the archive.
//
// Add ONE non-template function here and every consumer that includes an umbrella from two of
// its own TUs fails to link. Measured, on the shipped file, with two consumer TUs + the
// archive:
//
//     namespace qb { int actor_tpp_helper(int x) { return x + 1; } }
//     ->  duplicate symbol 'qb::actor_tpp_helper(int)' in: t2.o / t1.o
//         ld: 1 duplicate symbols
//
// The structure has existed since 2019-02-26 and has never fired, purely because nobody has
// added a non-template. The `.tpp` extension is the only guard, and it is a social one -- and
// the 3.0 restructure is scheduled to remove it (see dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0
// §3.1 and §5). Anything that must be non-template belongs in Actor.cpp.
//
// `inline` is NOT a workaround. It makes the link succeed and leaves N definitions of one
// entity in the program, which is the identity-duplication class this release spent a whole
// step fixing (see qb/utility/abi.h).
// ============================================================================================
#ifndef QB_ACTOR_TPL
#define QB_ACTOR_TPL

namespace qb {

template <callback_type _Actor>
void
Actor::registerCallback(_Actor &actor) const noexcept {
    VirtualCore::_handler->registerCallback(actor);
}

template <callback_type _Actor>
void
Actor::unregisterCallback(_Actor &actor) const noexcept {
    VirtualCore::_handler->unregisterCallback(actor.id());
}

template <event_type _Event, actor_type _Actor>
void
Actor::registerEvent(_Actor &actor) const noexcept {
    VirtualCore::_handler->registerEvent<_Event>(actor);
}

template <event_type _Event, actor_type _Actor>
void
Actor::unregisterEvent(_Actor &actor) const noexcept {
    VirtualCore::_handler->unregisterEvent<_Event>(actor);
}

template <typename _Event>
void
Actor::unregisterEvent() const noexcept {
    VirtualCore::_handler->unregisterEvent<_Event>(*this);
}

template <typename _Actor, typename... _Args>
ActorHandle<_Actor>
Actor::addRefActor(_Args &&...args) const {
    return ActorHandle<_Actor>(VirtualCore::_handler->template addReferencedActor<_Actor>(std::forward<_Args>(args)...));
}

template <typename _Actor>
_Actor *
ActorHandle<_Actor>::get() const noexcept {
    if (!_id.is_valid())
        return nullptr;
    // Must be resolved from the owning VirtualCore's worker thread.
    auto *handler = VirtualCore::_handler;
    if (!handler)
        return nullptr;
    // findActor is phase-aware: nullptr while the actor is still Activating, and after it
    // failed init / died. So `get()` returns a usable pointer only for an *active* actor.
    auto *resolved = handler->template findActor<_Actor>(_id);
    // Keep the cache fresh if it drifted (e.g. handle copied after kill).
    const_cast<ActorHandle<_Actor> *>(this)->_cached = resolved;
    return resolved;
}

template <typename _Event, typename... _Args>
_Event &
Actor::push(ActorId const &dest, _Args &&...args) const noexcept {
    return VirtualCore::_handler->template push<_Event>(dest, id(), std::forward<_Args>(args)...);
}

template <typename _Event, typename... _Args>
void
Actor::send(ActorId const &dest, _Args &&...args) const noexcept {
    VirtualCore::_handler->template send<_Event, _Args...>(dest, id(), std::forward<_Args>(args)...);
}

template <typename _Event, typename... _Args>
_Event
Actor::build_event(ActorId const source, _Args &&...args) const noexcept {
    _Event event{std::forward<_Args>(args)...};
    VirtualCore::fill_event(event, id(), source);
    return event;
}

template <typename _Required>
bool
Actor::require_type() const noexcept {
    broadcast<PingEvent>(type_id<_Required>());
    return true;
}

template <typename... _Types>
bool
Actor::require() const noexcept {
    return (require_type<_Types>() && ...);
}

template <typename _Event, typename... _Args>
void
Actor::broadcast(_Args &&...args) const noexcept {
    VirtualCore::_handler->template broadcast<_Event, _Args...>(id(), std::forward<_Args>(args)...);
}

template <typename Tag>
ActorId
Actor::getServiceId(CoreId const index) noexcept {
    // Route through `registerIndex<Tag>()` so that callers hitting this path
    // *before* `ServiceActor<Tag>::ServiceIndex` has been touched (rare, but
    // possible when DSOs are loaded lazily) still observe a valid, unique
    // service id rather than the `0` stamped by `unordered_map::operator[]`
    // on a fresh key (2.3).
    return {registerIndex<Tag>(), index};
}

template <typename _ServiceActor>
_ServiceActor *
Actor::getService() const noexcept {
    return VirtualCore::_handler->getService<_ServiceActor>();
}

template <event_type _Event, typename... _Args>
Actor::EventBuilder &
Actor::EventBuilder::push(_Args &&...args) noexcept {
    dest_pipe.push<_Event>(std::forward<_Args>(args)...);
    return *this;
}

template <typename Tag>
ServiceId
Actor::registerIndex() noexcept {
    // Magic static — the C++ standard guarantees that the initializer runs
    // exactly once, even if multiple threads race on first call. This fully
    // serialises the `_nb_service` increment and the insertion into the
    // registration map, fixing the non-atomic mutation reported in 2.3.
    static const ServiceId idx = [] {
        const auto                  new_id = VirtualCore::_nb_service.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(VirtualCore::servicesMutex());
        VirtualCore::getServices()[type_id<Tag>()] = new_id;
        return new_id;
    }();
    return idx;
}

// CoroContext implementation (needs VirtualCore access)

template <typename _Event, typename... Args>
void
CoroContext::push(Args &&...args) const {
    VirtualCore::_handler->template push<_Event>(actor_id_, actor_id_, std::forward<Args>(args)...);
}

template <typename _Event, typename... Args>
void
CoroContext::push_to(ActorId dest, Args &&...args) const {
    VirtualCore::_handler->template push<_Event>(dest, actor_id_, std::forward<Args>(args)...);
}

template <typename _Event, typename... Args>
void
CoroContext::broadcast(Args &&...args) const {
    VirtualCore::_handler->template broadcast<_Event>(actor_id_, std::forward<Args>(args)...);
}

template <typename E>
bool
Actor::resolve_ask(E &e) const noexcept {
    return qb::detail::ask_deliver(e.correlation_id, id(), e);
}

// Coroutine support implementation

namespace detail {

/**
 * @brief RAII guard that decrements the actor's active-coroutine counter when a
 *        wrapper frame completes or is destroyed (even after the owning actor is
 *        gone — the shared_ptr keeps the counter alive). Shared by both spawn
 *        wrappers so the decrement logic lives in one place.
 */
struct coro_count_guard {
    std::shared_ptr<std::atomic<std::size_t>> c;
    ~coro_count_guard() {
        c->fetch_sub(1, std::memory_order_relaxed);
    }
};

/**
 * Wrapper coroutine that owns the user's callable and the CoroContext
 * by value inside the coroutine frame. This prevents the "dangling lambda"
 * problem: when spawn_detached() receives a temporary lambda, the closure is
 * destroyed after the call — but the coroutine frame (which holds `func`
 * and `ctx` as value parameters) keeps them alive across all suspension
 * points.
 *
 * The shared_ptr<atomic> counter is decremented via RAII when the wrapper
 * completes or is destroyed (even if the owning actor has been deleted).
 *
 * Finding 2.D.6 — decision log:
 *   Two coroutine frames per spawn_detached are inherent to the design
 *   (wrapper frame + user body frame). We **intentionally** keep the
 *   wrapper because:
 *     1. Frame allocation cost is already amortised by the thread-local
 *        pooled allocator (Finding 2.A.9) — a steady-state spawn/despawn
 *        cycle burns zero `malloc`/`free`.
 *     2. Removing the wrapper would require either embedding a completion
 *        hook in every `task<void>::promise_type` (taxes all coroutines
 *        in the codebase, even non-actor ones) or a scheduler-side
 *        completion map (extra hash lookup per frame teardown).
 *     3. The wrapper is the single point that enforces actor lifetime
 *        safety (counter RAII + value-capture of func/ctx).
 */
template <typename Func>
qb::io::async::task<void>
actor_coro_wrapper(Func func, CoroContext ctx, std::shared_ptr<std::atomic<std::size_t>> counter) {
    coro_count_guard guard{std::move(counter)};
    co_await func(ctx);
}

/**
 * Wrapper for `spawn`. Same ownership / counter-RAII semantics as
 * `actor_coro_wrapper`, but it **swallows `qb::io::async::cancelled_error`** — the
 * expected signal when the actor's coroutine scope is cancelled on kill/destroy, so a
 * scoped coroutine being torn down is not surfaced as an error. Templated on the
 * context type so it accepts a `ScopedCoroContext` by value (kept alive in the frame).
 */
template <typename Func, typename Ctx>
qb::io::async::task<void>
actor_scoped_coro_wrapper(Func func, Ctx ctx, std::shared_ptr<std::atomic<std::size_t>> counter) {
    coro_count_guard guard{std::move(counter)};
    try {
        co_await func(ctx);
    } catch (const qb::io::async::cancelled_error &) {
        // Expected on actor-scope cancellation (kill/destroy) — swallow.
    }
}

} // namespace detail

template <typename Func>
void
Actor::spawn_detached(Func &&func) const {
    __resolve_coro_scheduler__();

    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    CoroContext ctx(this);

    // actor_coro_wrapper takes func BY VALUE → stored in the coroutine frame.
    // The scheduler takes ownership of the wrapper task's handle via spawn().
    coro_scheduler_->spawn(detail::actor_coro_wrapper(std::forward<Func>(func), ctx, active_coroutines_));
}

template <typename Func>
void
Actor::spawn(Func &&func) const {
    __resolve_coro_scheduler__();
    __ensure_coro_scope__(); // lazily allocate the real cancellation token on first use.

    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    // ScopedCoroContext carries the actor id + a copy of the scope token; the wrapper
    // stores it by value in the coroutine frame, so the token's shared state safely
    // outlives the actor.
    ScopedCoroContext ctx(this, _coro_scope);

    coro_scheduler_->spawn(detail::actor_scoped_coro_wrapper(std::forward<Func>(func), std::move(ctx), active_coroutines_));
}

} // namespace qb

#endif
