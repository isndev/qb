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

    template<typename _Actor, typename ..._Args>
    ActorId Main::addActor(std::size_t cid, _Args &&...args) noexcept {
        const auto index = static_cast<CoreId>(cid);
        auto it = _core_set.raw().find(index);
        ActorId id = ActorId::NotFound;
        if (!Main::is_running && it != _core_set.raw().end()) {
            if constexpr (std::is_base_of<Service, _Actor>::value)
                id = ActorId(_Actor::ServiceIndex, index);
            else
                id = ActorId(generated_sid++, index);
            auto fac = _actor_factories[index].find(id);
            if (fac == _actor_factories[index].end())
                _actor_factories[index].insert({id, new TActorFactory<_Actor, _Args...>(id, std::forward<_Args>(args)...)});
            else
                id = ActorId::NotFound;
        }
        return id;
    }

    template<typename _Actor, typename ..._Args>
    Main::CoreBuilder &Main::CoreBuilder::addActor(_Args &&...args) noexcept {
        auto id = _main.template addActor<_Actor, _Args...>(_index, std::forward<_Args>(args)...);
        if (id == ActorId::NotFound)
            _valid = false;

        _ret_ids.push_back(id);
        return *this;
    }

} // namespace qb

#endif //QB_MAIN_TPL