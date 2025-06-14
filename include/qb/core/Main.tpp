/**
 * @file qb/core/Main.tpp
 * @brief Template implementation for the Main class
 *
 * This file contains the template implementation of the Main class methods and related
 * classes defined in Main.h. It provides the actual implementation of actor creation,
 * core initialization, and system configuration functionality.
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

#include "ActorId.h"
#include "Main.h"
#include "VirtualCore.h"

#ifndef QB_MAIN_TPL
#define QB_MAIN_TPL

namespace qb {
class Main;

template <typename _Actor, typename... _Args>
ActorId
CoreInitializer::addActor(_Args &&...args) noexcept {
    ActorId id = ActorId::NotFound;
    if constexpr (std::is_base_of_v<Service, _Actor>) {
        if (_registered_services.find(_Actor::ServiceIndex) ==
            _registered_services.end()) {
            _registered_services.insert(_Actor::ServiceIndex);
            id = ActorId(_Actor::ServiceIndex, _index);
        } else {
            LOG_CRIT("[Start Sequence] Failed to add Service Actor("
                     << typeid(_Actor).name() << ")"
                     << " in Core(" << _index << ")"
                     << " : Already registered");
            return id;
        }
    } else {
        if (unlikely(_next_id == std::numeric_limits<ServiceId>::max())) {
            LOG_CRIT("[Start Sequence] Failed to add Actor("
                     << typeid(_Actor).name() << ")"
                     << " in Core(" << _index << ")"
                     << " : Max number of Actors reached");
            return id;
        }
        id = ActorId(_next_id++, _index);
    }
    _actor_factories.push_back(
        new TActorFactory<_Actor, _Args...>(id, std::forward<_Args>(args)...));
    return id;
}

template <typename _Actor, typename... _Args>
CoreInitializer::ActorBuilder &
CoreInitializer::ActorBuilder::addActor(_Args &&...args) noexcept {
    auto id =
        _initializer.template addActor<_Actor, _Args...>(std::forward<_Args>(args)...);
    if (!id.is_valid())
        _valid = false;

    _ret_ids.push_back(id);
    return *this;
}

template <typename _Actor, typename... _Args>
ActorId
Main::addActor(CoreId const cid, _Args &&...args) {
    return core(cid).addActor<_Actor>(std::forward<_Args>(args)...);
}

} // namespace qb

#endif // QB_MAIN_TPL