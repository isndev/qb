//
// Created by isndev on 12/4/18.
//

#include "Actor.h"
#include "Core.h"

namespace cube {

    Actor::Actor() {
        _event_map.reserve(64);
        this->template registerEvent<Event>(*this);
        this->template registerEvent<KillEvent>(*this);
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

    void Actor::on(Event const &event) {
        LOG_WARN << "Actor[" << this->id()._id << "." << this->id()._index << "] received removed event[" << event.id << "]";
    }

    void Actor::on(KillEvent const &) {
        kill();
    }

    auto Actor::getPipe(ActorId const dest) const {
        return _handler->getPipeProxy(dest, this->id());
    }

    uint16_t Actor::getIndex() const {
        return _handler->getIndex();
    }

    void Actor::unregisterCallback() const {
        _handler->unregisterCallback(this->id());
    }

    void Actor::kill() const {
        _handler->killActor(this->id());
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