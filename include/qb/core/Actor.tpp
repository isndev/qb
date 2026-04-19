/**
 * @file qb/core/Actor.tpp
 * @brief Template implementation for the Actor class
 *
 * This file contains the template implementation of the Actor class methods defined
 * in Actor.h. It provides the actual implementation of event handling, actor creation,
 * and inter-actor communication mechanisms.
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
 * @ingroup Core
 */

#include "VirtualCore.h"
#include "VirtualCore.tpp"

#include <cassert>

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
_Actor *
Actor::addRefActor(_Args &&...args) const {
    return VirtualCore::_handler->template addReferencedActor<_Actor>(
        std::forward<_Args>(args)...);
}

template <typename _Actor>
_Actor *
RefActorHandle<_Actor>::get() const noexcept {
    if (!_id.is_valid())
        return nullptr;
    // Must be resolved from the owning VirtualCore's worker thread.
    auto *handler = VirtualCore::_handler;
    if (!handler)
        return nullptr;
    auto *resolved = handler->template findActor<_Actor>(_id);
    // Keep the cache fresh if it drifted (e.g. handle copied after kill).
    const_cast<RefActorHandle<_Actor> *>(this)->_cached = resolved;
    return resolved;
}

template <typename _Event, typename... _Args>
_Event &
Actor::push(ActorId const &dest, _Args &&...args) const noexcept {
    return VirtualCore::_handler->template push<_Event>(dest, id(),
                                                        std::forward<_Args>(args)...);
}

template <typename _Event, typename... _Args>
void
Actor::send(ActorId const &dest, _Args &&...args) const noexcept {
    VirtualCore::_handler->template send<_Event, _Args...>(dest, id(),
                                                           std::forward<_Args>(args)...);
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
    VirtualCore::_handler->template broadcast<_Event, _Args...>(
        id(), std::forward<_Args>(args)...);
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
        const auto new_id = VirtualCore::_nb_service.fetch_add(
                                1, std::memory_order_relaxed) +
                            1;
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
    VirtualCore::_handler->template push<_Event>(actor_id_, actor_id_,
                                                 std::forward<Args>(args)...);
}

template <typename _Event, typename... Args>
void
CoroContext::push_to(ActorId dest, Args &&...args) const {
    VirtualCore::_handler->template push<_Event>(dest, actor_id_,
                                                 std::forward<Args>(args)...);
}

// Coroutine support implementation

namespace detail {

/**
 * Wrapper coroutine that owns the user's callable and the CoroContext
 * by value inside the coroutine frame. This prevents the "dangling lambda"
 * problem: when spawn_async() receives a temporary lambda, the closure is
 * destroyed after the call — but the coroutine frame (which holds `func`
 * and `ctx` as value parameters) keeps them alive across all suspension
 * points.
 *
 * The shared_ptr<atomic> counter is decremented via RAII when the wrapper
 * completes or is destroyed (even if the owning actor has been deleted).
 *
 * Finding 2.D.6 — decision log:
 *   Two coroutine frames per spawn_async are inherent to the design
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
qb::io::async::task<void> actor_coro_wrapper(
    Func func,
    CoroContext ctx,
    std::shared_ptr<std::atomic<std::size_t>> counter) {
    struct Guard {
        std::shared_ptr<std::atomic<std::size_t>> c;
        ~Guard() { c->fetch_sub(1, std::memory_order_relaxed); }
    } guard{std::move(counter)};
    co_await func(ctx);
}

} // namespace detail

template <typename Func>
void Actor::spawn_async(Func&& func) const {
    // Finding 2.D.4: revalidate the cached scheduler pointer on every spawn
    // by comparing it against the current TLS scheduler. Caching alone is
    // unsafe because the listener-owned scheduler can be torn down and
    // rebuilt (e.g. tests calling `listener::reset_coro_scheduler()`, or a
    // core that is destroyed and later re-initialized). Revalidation is
    // essentially free: `current_ptr()` is a plain `thread_local*` load,
    // and the comparison fits in one cmp+jne.
    auto* expected = qb::io::async::CoroutineScheduler::current_ptr();
    if (likely(expected != nullptr)) {
        if (unlikely(coro_scheduler_ != expected)) {
            coro_scheduler_ = expected;
        }
    } else if (unlikely(!coro_scheduler_)) {
        // Extremely rare path: spawn called before any TLS scheduler exists
        // on this thread. Fall back to the listener to create one.
        coro_scheduler_ = &qb::io::async::listener::current.coro_scheduler();
    }

    // Finding 2.D.5: debug-only guard against cross-thread spawn. The actor
    // system is strictly mono-thread per VirtualCore; calling spawn_async
    // from another thread would interleave with the owner's scheduler and
    // is UB. We can't differentiate which VirtualCore owns this actor at
    // this layer, but we CAN verify a TLS scheduler exists on the caller
    // thread, which excludes calls from unrelated std::thread contexts.
    assert(qb::io::async::CoroutineScheduler::current_ptr() != nullptr
           && "Actor::spawn_async called from a thread without a coroutine "
              "scheduler — are you calling this from outside the VirtualCore?");

    active_coroutines_->fetch_add(1, std::memory_order_relaxed);
    CoroContext ctx(this);

    // actor_coro_wrapper takes func BY VALUE → stored in the coroutine frame.
    // The scheduler takes ownership of the wrapper task's handle via spawn().
    coro_scheduler_->spawn(
        detail::actor_coro_wrapper(std::forward<Func>(func), ctx, active_coroutines_)
    );
}

} // namespace qb

#endif
