/**
 * @file qb/core/src/Actor.cpp
 * @brief Implementation of the Actor class for the QB framework
 *
 * This file contains the implementation of the Actor class which forms the foundation
 * of the actor model in the QB framework. It includes event handling, actor lifecycle
 * management, and inter-actor communication mechanisms.
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

#include <map>
#include <qb/core/Actor.h>
#include <qb/core/Actor.tpp>
#include <qb/core/VirtualCore.h>

namespace qb {

Actor::Actor() noexcept
    : _id(VirtualCore::_handler->__generate_id__()) {
    registerEvent<KillEvent>(*this);
    registerEvent<SignalEvent>(*this);
    registerEvent<UnregisterCallbackEvent>(*this);
    registerEvent<PingEvent>(*this);
}

Actor::Actor(ActorId const id) noexcept
    : _id(id) {
    registerEvent<KillEvent>(*this);
    registerEvent<SignalEvent>(*this);
    registerEvent<UnregisterCallbackEvent>(*this);
    registerEvent<PingEvent>(*this);
}

void
Actor::on(PingEvent const &event) noexcept {
    if (event.type == id_type)
        send<RequireEvent>(event.source, event.type, ActorStatus::Alive);
}

void
Actor::on(KillEvent const &) noexcept {
    kill();
}

void
Actor::on(SignalEvent const &event) noexcept {
    if (event.signum == SIGINT)
        kill();
}

void
Actor::on(UnregisterCallbackEvent const &) noexcept {
    VirtualCore::_handler->__unregisterCallback(id());
}

uint64_t
Actor::time() const noexcept {
    return VirtualCore::_handler->time();
}

bool
Actor::is_alive() const noexcept {
    return _alive;
}

Pipe
Actor::getPipe(ActorId const dest) const noexcept {
    return VirtualCore::_handler->getProxyPipe(dest, id());
}

CoreId
Actor::getIndex() const noexcept {
    return VirtualCore::_handler->getIndex();
}

std::string_view
Actor::getName() const noexcept {
    return name;
}

const CoreIdSet &
Actor::getCoreSet() const noexcept {
    return VirtualCore::_handler->getCoreSet();
}

void
Actor::unregisterCallback() const noexcept {
    VirtualCore::_handler->unregisterCallback(id());
}

void
Actor::kill() const noexcept {
    _alive = false;
    VirtualCore::_handler->killActor(id());
}

Actor::EventBuilder::EventBuilder(Pipe const &pipe) noexcept
    : dest_pipe(pipe) {}

Actor::EventBuilder
Actor::to(ActorId const dest) const noexcept {
    return EventBuilder{getPipe(dest)};
}

void
Actor::reply(Event &event) const noexcept {
    if (unlikely(event.dest.is_broadcast())) {
        LOG_WARN(*this << " failed to reply broadcast event");
        return;
    }
    VirtualCore::_handler->reply(event);
}

void
Actor::forward(ActorId const dest, Event &event) const noexcept {
    event.source = id();
    if (unlikely(event.dest.is_broadcast())) {
        LOG_WARN(*this << " failed to forward broadcast event");
        return;
    }
    VirtualCore::_handler->forward(dest, event);
}

// OpenApi : internal future use
void
Actor::send(Event const &event) const noexcept {
    VirtualCore::_handler->send(event);
}

void
Actor::push(Event const &event) const noexcept {
    VirtualCore::_handler->push(event);
}

bool
Actor::try_send(Event const &event) const noexcept {
    return VirtualCore::_handler->try_send(event);
}

Service::Service(ServiceId const sid)
    : Actor(ActorId(sid, VirtualCore::_handler->getIndex())) {}
} // namespace qb

#ifdef QB_LOGGER
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "."
       << actor.id().sid() << ")";
    return os;
}
#endif

std::ostream &
qb::operator<<(std::ostream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "."
       << actor.id().sid() << ")";
    return os;
}