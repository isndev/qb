#include "ActorId.h"
#include "Core.h"
#include "Main.h"

#ifndef CUBE_CUBE_TPL
#define CUBE_CUBE_TPL

namespace cube {
    class Main;
    template<typename _Actor, typename ..._Init>
    ActorId Main::addActor(std::size_t index, _Init &&...init) {
        auto it = _cores.find(static_cast<uint8_t >(index));
        if (it != _cores.end()) {
            return it->second-> template addActor<_Actor, _Init...>
                    (index, std::forward<_Init>(init)...);
        }

        return ActorId::NotFound;
    }

    template<template<typename _Trait> typename _Actor, typename _Trait, typename ..._Init>
    ActorId Main::addActor(std::size_t index, _Init &&...init) {
        auto it = _cores.find(static_cast<uint8_t>(index));
        if (it != _cores.end()) {
            return it->second-> template addActor<_Actor, _Trait, _Init...>
                    (index, std::forward<_Init>(init)...);
        }

        return ActorId::NotFound;
    }
} // namespace cube

#endif //CUBE_CUBE_TPL