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

#include "ActorId.h"
#include "VirtualCore.h"
#include "Main.h"

#ifndef QB_MAIN_TPL
#define QB_MAIN_TPL

namespace qb {
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

} // namespace qb

#endif //QB_MAIN_TPL