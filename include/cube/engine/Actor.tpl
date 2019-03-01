#include "Core.h"
#include "Core.tpl"

#ifndef CUBE_ACTOR_TPL
# define CUBE_ACTOR_TPL

namespace cube {

    template<typename _Actor>
    void Actor::registerCallback(_Actor &actor) const {
        _handler->registerCallback(actor);
    }

    template<typename _Actor>
    void Actor::unregisterCallback(_Actor &actor) const {
        _handler->unregisterCallback(actor->id());
    }

    template<typename _Event, typename _Actor>
    void Actor::registerEvent(_Actor &actor) {
        auto it = _event_map.find(type_id<_Event>());
        if (it != _event_map.end())
            delete it->second;
        _event_map.insert_or_assign(type_id<_Event>(), new RegisteredEvent<_Event, _Actor>(actor));
    }

    template<typename _Event, typename _Actor>
    void Actor::unregisterEvent(_Actor &actor) {
        auto it = _event_map.find(type_id<_Event>());
        if (it != _event_map.end())
            delete it->second;
        _event_map.insert_or_assign(type_id<_Event>(), new RegisteredEvent<Event, _Actor>(actor));
    }

    template<typename _Event>
    void Actor::unregisterEvent() {
        unregisterEvent<_Event>(*this);
    }

    template<typename _Actor, typename ..._Args>
    _Actor *Actor::addRefActor(_Args &&...args) const {
        return _handler->template addReferencedActor<_Actor>(std::forward<_Args>(args)...);
    }

    template<typename _Event, typename ..._Args>
    _Event &Actor::push(ActorId const &dest, _Args &&...args) const {
        return _handler->template push<_Event>(dest, id(), std::forward<_Args>(args)...);
    }

    //    template<typename _Event, typename ..._Args>
    //    void Actor::fast_push(ActorId const &dest, _Args const &...args) const {
    //    TODO: find a way to implement this
    //          _handler->template fast_push<_Event>(dest, id(), args...);
    //    }

    template<typename _Event, typename ..._Args>
    void Actor::send(ActorId const &dest, _Args &&...args) const {
        _handler->template send<_Event, _Args...>(dest, id(), std::forward<_Args>(args)...);
    }

    template <typename T>
    ActorId Actor::getServiceId(uint16_t const index) const {
        return {T::sid, index};
    }

    template<typename _Event, typename ..._Args>
    Actor::EventBuilder &Actor::EventBuilder::push(_Args &&...args) {
        dest_pipe.push<_Event>(std::forward<_Args>(args)...);
        return *this;
    }

}

#endif