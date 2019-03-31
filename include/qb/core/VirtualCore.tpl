/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include "VirtualCore.h"

#ifndef QB_CORE_TPL
# define QB_CORE_TPL

namespace qb {

    template<typename _Event, typename _Actor>
    void VirtualCore::registerEvent(_Actor &actor) noexcept {
        using handler_type = EventHandler<_Event>;
        using event_type = typename handler_type::template RegisteredEvent<_Actor>;
        auto ievent = new event_type(actor);
        auto it = _event_map.find(qb::type_id<_Event>());
        if (it == _event_map.end())
            _event_map.insert({qb::type_id<_Event>(), new handler_type{}}).first->second->registerEvent(ievent);
        else
            it->second->registerEvent(ievent);
    }

    template<typename _Event, typename _Actor>
    void VirtualCore::unregisterEvent(_Actor &actor) noexcept {
        auto it = _event_map.find(qb::type_id<_Event>());
        if (it != _event_map.end()) {
            it->second->unregisterEvent(actor.id());
        } else {
            LOG_WARN("" << *this << "Failed to unregister event");
        }
    }

    template<typename _Actor, typename ..._Init>
    _Actor *VirtualCore::addReferencedActor(_Init &&...init) noexcept {
        auto actor = new _Actor(std::forward<_Init>(init)...);
        actor->id_type = type_id<_Actor>();
        if (appendActor(*actor, std::is_base_of<Service, _Actor>::value, true) != ActorId::NotFound)
            return actor;
        return nullptr;
    };

    template <typename _Actor>
    void VirtualCore::registerCallback(_Actor &actor) noexcept {
        _actor_callbacks.insert({actor.id(), &actor});
    }

    // Event API
    template <typename T>
    inline void VirtualCore::fill_event(T &data, ActorId const dest, ActorId const source) const noexcept {
        data.id = type_id<T>();
        data.dest = dest;
        data.source = source;

        if constexpr (std::is_base_of<ServiceEvent, T>::value) {
            data.forward = source;
            std::swap(data.id, data.service_event_id);
        }

        data.bucket_size = static_cast<uint16_t>(allocator::getItemSize<T, CacheLine>());
    }

    template<typename T, typename ..._Init>
    void VirtualCore::send(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
        auto &pipe = __getPipe__(dest._index);
        auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);

        fill_event(data, dest, source);

        if (try_send(data))
            pipe.free(data.bucket_size);
    }


    template<typename T, typename ..._Init>
    void VirtualCore::broadcast(ActorId const source, _Init &&...init) noexcept {
        for (const auto it : _engine._core_set.raw())
            send<T, _Init...>(BroadcastId(it), source, std::forward<_Init>(init)...);
    }

    template<typename T, typename ..._Init>
    T &VirtualCore::push(ActorId const dest, ActorId const source, _Init &&...init) noexcept {
        auto &pipe = __getPipe__(dest._index);
        auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);

        fill_event(data, dest, source);

        return data;
    }
    //!Event Api

    template <typename Tag>
    const ServiceId ServiceActor<Tag>::ServiceIndex = Actor::registerIndex<Tag>();

}

#endif