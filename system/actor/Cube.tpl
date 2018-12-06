#include "ActorId.h"
#include "Core.h"
#include "Cube.h"

#ifndef CUBE_CUBE_TPL
#define CUBE_CUBE_TPL

namespace cube {
    class Cube;
    template<typename _Actor, typename ..._Init>
    ActorId Cube::addActor(std::size_t index, _Init &&...init) {
        auto it = _cores.find(index);
        if (it != _cores.end()) {
            return it->second-> template addActor<_Actor, _Init...>
                    (index, std::forward<_Init>(init)...);
        }

        return ActorId::NotFound{};
    }

    template<template<typename _Trait> typename _Actor, typename _Trait, typename ..._Init>
    ActorId Cube::addActor(std::size_t index, _Init &&...init) {
        auto it = _cores.find(index);
        if (it != _cores.end()) {
            return it->second-> template addActor<_Actor, _Trait, _Init...>
                    (index, std::forward<_Init>(init)...);
        }

        return ActorId::NotFound{};
    }
}

#endif //CUBE_CUBE_TPL