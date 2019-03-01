//
// Created by isndev on 12/4/18.
//

#include <cube/engine/Actor.h>
#include <cube/engine/Actor.tpl>
#include <cube/engine/Core.h>

namespace cube {

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

cube::io::stream &operator<<(cube::io::stream &os, cube::Actor const &actor){
    std::stringstream ss;
    ss << "Actor(" << actor.id().index() << "." << actor.id().sid() << ")";
    os << ss.str();
    return os;
}