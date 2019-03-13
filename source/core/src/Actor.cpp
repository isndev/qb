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

    Actor::Actor() {
        _event_map.reserve(64);
        registerEvent<Event>(*this);
        registerEvent<KillEvent>(*this);
    }

    Actor::~Actor() {
        for (const auto &revent : _event_map)
            delete revent.second;
    }

    void Actor::on(Event *event) const {
        // TODO: secure this if event not registred
        // branch fetch find
        _event_map.at(event->id)->invoke(event);
    }

    void Actor::__set_id(ActorId const &id) {
        static_cast<ActorId &>(*this) = id;
    }

    void Actor::__set_id(uint16_t const sid, uint16_t const cid) {
        static_cast<ActorId &>(*this) = {sid, cid};
    }

    void Actor::on(Event const &event) {
        LOG_WARN << *this << " received removed event[" << event.id << "]";
    }

    void Actor::on(KillEvent const &) {
        kill();
    }

    uint64_t Actor::time() const {
        return _handler->time();
    }

    bool Actor::isAlive() const {
        return _alive;
    }

    ProxyPipe Actor::getPipe(ActorId const dest) const {
        return _handler->getProxyPipe(dest, id());
    }

    uint16_t Actor::getIndex() const {
        return _handler->getIndex();
    }

    void Actor::unregisterCallback() const {
        _handler->unregisterCallback(id());
    }

    void Actor::kill() const {
        _alive = false;
        _handler->killActor(id());
    }

    Actor::EventBuilder::EventBuilder(ProxyPipe const &pipe)
        : dest_pipe(pipe) {}

    Actor::EventBuilder Actor::to(ActorId const dest) const {
        return {getPipe(dest)};
    }

    void Actor::reply(Event &event) const {
        _handler->reply(event);
    }

    void Actor::forward(ActorId const dest, Event &event) const {
        _handler->forward(dest, event);
    }

    void Actor::send(Event const &event) const {
        _handler->send(event);
    }

    void Actor::push(Event const &event) const {
        _handler->push(event);
    }

    bool Actor::try_send(Event const &event) const {
        return _handler->try_send(event);
    }
}

qb::io::stream &operator<<(qb::io::stream &os, qb::Actor const &actor){
    std::stringstream ss;
    ss << "Actor(" << actor.id().index() << "." << actor.id().sid() << ")";
    os << ss.str();
    return os;
}