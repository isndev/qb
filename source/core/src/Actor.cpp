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

#include <map>
#include <qb/core/Actor.h>
#include <qb/core/Actor.tpl>
#include <qb/core/VirtualCore.h>

namespace qb {

    void Actor::__set_id(ActorId const &id) noexcept {
        static_cast<ActorId &>(*this) = id;
    }

    void Actor::__set_id(ServiceId const sid, CoreId const cid) noexcept {
        static_cast<ActorId &>(*this) = {sid, cid};
    }

    void Actor::on(PingEvent const &event) noexcept {
        if (event.type == id_type)
            send<RequireEvent>(event.source, event.type, ActorStatus::Alive);
    }

    void Actor::on(KillEvent const &) noexcept {
        kill();
    }

    uint64_t Actor::time() const noexcept {
        return VirtualCore::_handler->time();
    }

    bool Actor::isAlive() const noexcept{
        return _alive;
    }

    ProxyPipe Actor::getPipe(ActorId const dest) const noexcept {
        return VirtualCore::_handler->getProxyPipe(dest, id());
    }

    CoreId Actor::getIndex() const noexcept {
        return VirtualCore::_handler->getIndex();
    }

    void Actor::unregisterCallback() const noexcept {
        VirtualCore::_handler->unregisterCallback(id());
    }

    void Actor::kill() const noexcept {
        _alive = false;
        VirtualCore::_handler->killActor(id());
    }

    Actor::EventBuilder::EventBuilder(ProxyPipe const &pipe) noexcept
            : dest_pipe(pipe) {}

    Actor::EventBuilder Actor::to(ActorId const dest) const noexcept {
        return {getPipe(dest)};
    }

    void Actor::reply(Event &event) const noexcept {
        if (unlikely(event.dest.isBroadcast())) {
            LOG_WARN("" << *this << " failed to reply broadcast event");
            return;
        }
        VirtualCore::_handler->reply(event);
    }

    void Actor::forward(ActorId const dest, Event &event) const noexcept {
        event.source = id();
        if (unlikely(event.dest.isBroadcast())) {
            LOG_WARN("" << *this << " failed to forward broadcast event");
            return;
        }
        VirtualCore::_handler->forward(dest, event);
    }

    // OpenApi : internal future use
    void Actor::send(Event const &event) const noexcept {
        VirtualCore::_handler->send(event);
    }

    void Actor::push(Event const &event) const noexcept {
        VirtualCore::_handler->push(event);
    }

    bool Actor::try_send(Event const &event) const noexcept {
        return VirtualCore::_handler->try_send(event);
    }
}

qb::io::stream &operator<<(qb::io::stream &os, qb::Actor const &actor){
    std::stringstream ss;
    ss << "Actor(" << actor.id().index() << "." << actor.id().sid() << ")";
    os << ss.str();
    return os;
}