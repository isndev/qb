/**
 * @file qb/core/ActorId.cpp
 * @brief Implementation of the ActorId class for the QB framework
 *
 * This file contains the implementation of the ActorId class which provides
 * unique identifiers for actors in the QB framework, combining service ID
 * and core ID components.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <qb/core/ActorId.h>
#include <ostream>

// Accessors remain in-class on the per-event path. Only diagnostic stream output lives here.

std::ostream &
qb::operator<<(std::ostream &os, qb::ActorId const &id) {
    return os << id.index() << '.' << id.slot() << '@' << id.generation();
}

#ifdef QB_WITH_LOGGING
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::ActorId const &id) {
    std::stringstream ss;
    ss << id.index() << "." << id.slot() << "@" << id.generation();
    os << ss.str();
    return os;
}
#endif
