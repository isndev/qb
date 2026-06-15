/**
 * @file qb/core/VirtualCore.tpp
 * @brief Template implementation for the VirtualCore class
 *
 * This file contains the template implementation of the VirtualCore class methods
 * defined in VirtualCore.h. It provides the actual implementation of event routing,
 * actor management, and core communication functionality.
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

#ifndef QB_CORE_TPL
#define QB_CORE_TPL

namespace qb {

template <typename _Event, typename _Actor>
void
VirtualCore::registerEvent(_Actor &actor) noexcept {
    // Never subscribe an actor that failed id allocation (service-id pool
    // exhausted -> __generate_id__ returned NotFound). Its constructor still
    // runs registerEvent for the default events, but appendActor() rejects it
    // and destroys it without unsubscribing; a subscription keyed by the
    // NotFound id would then dangle and route to freed memory if any event is
    // sent to a default/NotFound ActorId.
    if (unlikely(!actor.id().is_valid()))
        return;
    LOG_INFO("Actor(" << actor.id() << ") subscribed to "
                      << ActorProxy::getName<_Event>());
    _router.subscribe<_Event>(actor);
}

template <typename _Event, typename _Actor>
void
VirtualCore::unregisterEvent(_Actor &actor) noexcept {
    LOG_INFO("Actor(" << actor.id() << ") unsubscribed to "
                      << ActorProxy::getName<_Event>());
    _router.unsubscribe<_Event>(actor);
}

template <typename _Actor, typename... _Init>
_Actor *
VirtualCore::addReferencedActor(_Init &&...init) noexcept {
    // Route through the same allocation customization point used by TActorFactory so
    // users who override qb::allocate_actor<_Actor> get consistent behaviour for
    // both engine-created and dynamically-added actors (PMR/pool support, 2.13).
    auto *raw_actor = qb::allocate_actor<_Actor>(std::forward<_Init>(init)...);
    std::unique_ptr<_Actor> actor_ptr(raw_actor);
    // Use the ActorProxy customization point so dynamically created actors get the
    // same demangled name and typed id_type as factory-created ones.
    ActorProxy::setTypeInfo<_Actor>(*raw_actor);
    if (appendActor(std::move(actor_ptr), true).is_valid())
        return raw_actor;
    return nullptr;
}

template <typename _Actor>
_Actor *
VirtualCore::findActor(ActorId const id) const noexcept {
    if (!id.is_valid())
        return nullptr;
    const auto it = _actors.find(id);
    if (it == _actors.end())
        return nullptr;
    Actor *raw = it->second.get();
    if (!raw->is_alive())
        return nullptr;
    // Fast path: id_type set by ActorProxy::setTypeInfo matches the requested type.
    if (raw->id_type == ActorProxy::getType<_Actor>())
        return static_cast<_Actor *>(raw);
    // Slow path: support downcasts to base classes derived from Actor.
    return dynamic_cast<_Actor *>(raw);
}

template <typename _ServiceActor>
_ServiceActor *
VirtualCore::getService() const noexcept {
    const auto &it = _actors.find(ActorId(_ServiceActor::ServiceIndex, _index));
    if (it == _actors.end()) {
        LOG_CRIT("Failed to get Service[" << typeid(_ServiceActor).name() << "]"
                                          << " in Core(" << _index << ")"
                                          << " : does not exist");
        return nullptr;
    }
    return dynamic_cast<_ServiceActor *>(it->second.get());
}

template <typename _Actor>
void
VirtualCore::registerCallback(_Actor &actor) noexcept {
    // Same guard as registerEvent: an actor that failed id allocation must not
    // leave a callback entry keyed by its NotFound id (it is about to be
    // destroyed by appendActor()).
    if (unlikely(!actor.id().is_valid()))
        return;
    auto [it, inserted] = _actor_callbacks.insert({actor.id(), &actor});
    if (inserted) {
        // Maintain the flat snapshot for the workflow loop (2.6).
        _callback_list.push_back({&actor, actor.id()});
    }
}

// Event API
template <typename T>
inline void
VirtualCore::fill_event(T &data, ActorId const dest, ActorId const source) noexcept {
    data.id     = data.template type_to_id<T>();
    data.dest   = dest;
    data.source = source;

    // C++23: Use event_qos0_type and service_event_type concepts
    if constexpr (event_qos0_type<T>) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "EventQOS < 2 require to be trivially destructible");
    }

    if constexpr (service_event_type<T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = static_cast<uint16_t>(allocator::getItemSize<T, EventBucket>());
}

template <typename T, typename... _Init>
void
VirtualCore::send(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
    auto &pipe = __getPipe__(dest._core_id);
    auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);

    fill_event(data, dest, source);

    if (dest._core_id != _index && try_send(data))
        pipe.free(data.bucket_size);
}

template <typename T, typename... _Init>
void
VirtualCore::broadcast(ActorId const source, _Init &&...init) noexcept {
    for (const auto it : _engine._core_set.raw())
        send<T, _Init...>(BroadcastId(it), source, std::forward<_Init>(init)...);
}

template <typename T, typename... _Init>
T &
VirtualCore::push(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
    auto &pipe = __getPipe__(dest._core_id);
    auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);

    fill_event(data, dest, source);

    return data;
}
//! Event Api

template <typename Tag>
inline const ServiceId ServiceActor<Tag>::ServiceIndex = Actor::registerIndex<Tag>();

} // namespace qb

#endif