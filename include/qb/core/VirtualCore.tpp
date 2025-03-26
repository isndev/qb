/**
 * @file qb/core/VirtualCore.tpp
 * @brief Template implementation for the VirtualCore class
 * 
 * This file contains the template implementation of the VirtualCore class methods defined
 * in VirtualCore.h. It provides the actual implementation of event routing, actor
 * management, and core communication functionality.
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
    LOG_INFO("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<_Event>());
    _router.subscribe<_Event>(actor);
}

template <typename _Event, typename _Actor>
void
VirtualCore::unregisterEvent(_Actor &actor) noexcept {
    LOG_INFO("Actor(" << actor.id() << ") unsubscribed to " << ActorProxy::getName<_Event>());
    _router.unsubscribe<_Event>(actor);
}

template <typename _Actor, typename... _Init>
_Actor *
VirtualCore::addReferencedActor(_Init &&... init) noexcept {
    auto actor_ptr = std::make_unique<_Actor>(std::forward<_Init>(init)...);
    _Actor* actor = actor_ptr.get();
    actor->id_type = type_id<_Actor>();
    actor->name = typeid(_Actor).name();
    if (appendActor(*actor, true).is_valid()) {
        // Transfert de propriété au système d'acteurs
        actor_ptr.release();
        return actor;
    }
    return nullptr;
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
    return dynamic_cast<_ServiceActor *>(it->second);
}

template <typename _Actor>
void
VirtualCore::registerCallback(_Actor &actor) noexcept {
    _actor_callbacks.insert({actor.id(), &actor});
}

// Event API
template <typename T>
inline void
VirtualCore::fill_event(T &data, ActorId const dest, ActorId const source) noexcept {
    data.id = data.template type_to_id<T>();
    data.dest = dest;
    data.source = source;

    if constexpr (std::is_base_of_v<EventQOS0, T>) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "EventQOS < 2 require to be trivially destructible");
    }

    if constexpr (std::is_base_of_v<ServiceEvent, T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = static_cast<uint16_t>(allocator::getItemSize<T, EventBucket>());
}

template <typename T, typename... _Init>
void
VirtualCore::send(ActorId const dest, ActorId const source, _Init &&... init) noexcept {
    auto &pipe = __getPipe__(dest._core_id);
    auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);

    fill_event(data, dest, source);

    if (dest._core_id != _index && try_send(data))
        pipe.free(data.bucket_size);
}

template <typename T, typename... _Init>
void
VirtualCore::broadcast(ActorId const source, _Init &&... init) noexcept {
    for (const auto it : _engine._core_set.raw())
        send<T, _Init...>(BroadcastId(it), source, std::forward<_Init>(init)...);
}

template <typename T, typename... _Init>
T &
VirtualCore::push(ActorId const dest, ActorId const source, _Init &&... init) noexcept {
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