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
#include "VirtualCore.tpl"

#ifndef QB_ACTOR_TPL
# define QB_ACTOR_TPL

namespace qb {

    template<typename _Actor>
    void Actor::registerCallback(_Actor &actor) const noexcept {
        VirtualCore::_handler->registerCallback(actor);
    }

    template<typename _Actor>
    void Actor::unregisterCallback(_Actor &actor) const noexcept {
        VirtualCore::_handler->unregisterCallback(actor.id());
    }

    template<typename _Event, typename _Actor>
    void Actor::registerEvent(_Actor &actor) const noexcept {
        VirtualCore::_handler->registerEvent<_Event>(actor);
    }

    template<typename _Event, typename _Actor>
    void Actor::unregisterEvent(_Actor &actor) const noexcept {
        VirtualCore::_handler->unregisterEvent<_Event>(actor);
    }

    template<typename _Event>
    void Actor::unregisterEvent() const noexcept {
        VirtualCore::_handler->unregisterEvent<_Event>(*this);
    }

    template<typename _Actor, typename ..._Args>
    _Actor *Actor::addRefActor(_Args &&...args) const {
        return VirtualCore::_handler->template addReferencedActor<_Actor>(std::forward<_Args>(args)...);
    }

    template<typename _Event, typename ..._Args>
    _Event &Actor::push(ActorId const &dest, _Args &&...args) const noexcept {
        return VirtualCore::_handler->template push<_Event>(dest, id(), std::forward<_Args>(args)...);
    }

    template<typename _Event, typename ..._Args>
    void Actor::send(ActorId const &dest, _Args &&...args) const noexcept {
        VirtualCore::_handler->template send<_Event, _Args...>(dest, id(), std::forward<_Args>(args)...);
    }

    template<typename _Required>
    bool Actor::require_type() const noexcept {
        broadcast<PingEvent>(type_id<_Required>());
        return true;
    }

    template<typename ..._Types>
    bool Actor::require() const noexcept {
        return (require_type<_Types>() && ...);
    }

    template<typename _Event, typename ..._Args>
    void Actor::broadcast(_Args &&... args) const noexcept {
        VirtualCore::_handler->template broadcast<_Event, _Args...>(id(), std::forward<_Args>(args)...);
    }

    template <typename Tag>
    ActorId Actor::getServiceId(CoreId const index) noexcept {
        return {VirtualCore::getServices()[type_id<Tag>()], index};
    }

    template<typename _Event, typename ..._Args>
    Actor::EventBuilder &Actor::EventBuilder::push(_Args &&...args) noexcept {
        dest_pipe.push<_Event>(std::forward<_Args>(args)...);
        return *this;
    }

    template <typename Tag>
    ServiceId Actor::registerIndex() noexcept {
        return VirtualCore::getServices()[type_id<Tag>()] = ++VirtualCore::_nb_service;
    }

}

#endif