#include "Core.h"

#ifndef CUBE_ACTOR_TPL
# define CUBE_ACTOR_TPL

namespace cube {

    template<typename _Actor>
    void Actor::registerCallback(_Actor &actor) const {
        _handler->registerCallback(actor);
    }

    template<typename _Actor, typename ..._Init>
    auto Actor::addRefActor(_Init &&...init) const {
        return _handler->template addReferencedActor<_Actor>(std::forward<_Init>(init)...);
    }

    template<template<typename _Trait> typename _Actor, typename _Trait, typename ..._Init>
    auto Actor::addRefActor(_Init &&...init) const {
        return _handler->template addReferencedActor<_Actor, _Trait>(std::forward<_Init>(init)...);
    }

    template<typename _Data, typename ..._Init>
    _Data &Actor::push(ActorId const &dest, _Init const &...init) const {
        return _handler->template push<_Data>(dest, this->id(), init...);
    }

    template<typename _Data, typename ..._Init>
    _Data &Actor::fast_push(ActorId const &dest, _Init const &...init) const {
        return _handler->template fast_push<_Data>(dest, this->id(), init...);
    }

    template<typename _Data, typename ..._Init>
    void Actor::send(ActorId const &dest, _Init &&...init) const {
        _handler->template send<_Data, _Init...>(dest, this->id(), std::forward<_Init>(init)...);
    }

}

#endif