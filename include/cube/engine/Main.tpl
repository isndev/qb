#include "ActorId.h"
#include "Core.h"
#include "Main.h"

#ifndef CUBE_MAIN_TPL
#define CUBE_MAIN_TPL

namespace cube {
    class Main;

    template<typename _Actor, typename ..._Init>
    ActorId Main::addActor(std::size_t index, _Init &&...init) {
        auto it = _cores.find(static_cast<uint8_t >(index));
        if (!Main::is_running && it != _cores.end()) {
            return it->second-> template addActor<_Actor, _Init...>
                    (std::forward<_Init>(init)...);
        }

        return ActorId::NotFound;
    }

    template<typename _Actor, typename ..._Args>
    Main::CoreBuilder &Main::CoreBuilder::addActor(_Args &&...args) {
        auto id = _main.template addActor<_Actor, _Args...>(_index, std::forward<_Args>(args)...);
        if (id == ActorId::NotFound)
            _valid = false;

        _ret_ids.push_back(id);
        return *this;
    }

} // namespace cube

#endif //CUBE_MAIN_TPL