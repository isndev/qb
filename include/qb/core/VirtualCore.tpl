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

    template<typename _Actor>
    bool VirtualCore::initActor(_Actor * const actor, bool doinit) {
        if constexpr (!std::is_base_of<Service, _Actor>::value) {
            auto id = __generate_id__();
            actor->__set_id(id);
            // Number of actors attends to its limit in this core
            if (id == ActorId::NotFound || (doinit && unlikely(!actor->onInit()))) {
                _ids.insert(static_cast<uint16_t>(id));
                delete actor;
                return false;
            }
        } else {
            actor->_index = _index;
            if (_actors.find(actor->id()) != _actors.end() || (doinit && unlikely(!actor->onInit()))) {
                delete actor;
                return false;
            }
        }
        return true;
    }

    template<typename _Actor, typename ..._Init>
    ActorId VirtualCore::addActor(_Init &&...init) {
        auto actor = new _Actor(std::forward<_Init>(init)...);
        actor->_handler = this;

        if (!initActor(actor, false))
            return ActorId::NotFound;

        addActor(actor);
        return actor->id();
    };

    template<typename _Actor, typename ..._Init>
    _Actor *VirtualCore::addReferencedActor(_Init &&...init) {
        auto actor = new _Actor(std::forward<_Init>(init)...);
        actor->_handler = this;

        if (!initActor(actor, true))
            return nullptr;

        addActor(actor);
        return actor;
    };

    template <typename _Actor>
    void VirtualCore::registerCallback(_Actor &actor) {
        _actor_callbacks.insert({actor.id(), &actor});
    }

    // Event API
    template <typename T>
    inline void VirtualCore::fill_event(T &data, ActorId const dest, ActorId const source) const {
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
    void VirtualCore::send(ActorId const dest, ActorId const source, _Init &&...init) {
        auto &pipe = __getPipe__(dest._index);
        auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);

        fill_event(data, dest, source);

        if (try_send(data))
            pipe.free(data.bucket_size);
    }

    template<typename T, typename ..._Init>
    T &VirtualCore::push(ActorId const dest, ActorId const source, _Init &&...init) {
        auto &pipe = __getPipe__(dest._index);
        auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);

        fill_event(data, dest, source);

        return data;
    }

    template<typename T, typename ..._Init>
    void VirtualCore::fast_push(ActorId const dest, ActorId const source, _Init &&...init) {
//        auto &pipe = __getPipe__(dest._index);
//        auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);
//
//        fill_event(data, dest, source);
//
//        if (likely(try_send(data)))
//            pipe.free_back(data.bucket_size);
    }
    //!Event Api

	    template <typename Tag>
    const uint16_t ServiceActor<Tag>::ServiceIndex = Actor::registerIndex<Tag>();

}

#endif