/**
 * @file qb/core/CoreSet.cpp
 * @brief Implementation of the CoreSet class for managing core IDs
 *
 * This file contains the implementation of the CoreSet class which manages
 * sets of core IDs and provides functionality for mapping between logical
 * and physical core IDs in the QB Actor Framework.
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

#include <limits>
#include <algorithm>
#include <numeric>
#include <qb/core/CoreSet.h>

namespace qb {

CoreSet::CoreSet(CoreIdSet const &set) noexcept
    : _raw_set(set)
    , _nb_core(set.size())
    , _size([&set]() {
        // Modern C++: find maximum ID using auto for proper type deduction
        for (auto i = static_cast<int>(MaxCores) - 1; i >= 0; --i) {
            if (set.contains(static_cast<CoreId>(i))) {
                return static_cast<std::size_t>(i + 1);
            }
        }
        return static_cast<std::size_t>(0);
    }()) {
    uint8_t idx = 0;
    for (auto id : set) {
        _set[id] = idx++;
    }
}

// resolve() is defined in-class (CoreSet.h): it is on the per-push hot path.

uint32_t
CoreSet::getSize() const noexcept {
    return _size;
}

uint32_t
CoreSet::getNbCore() const noexcept {
    return _nb_core;
}

const CoreIdSet &
CoreSet::raw() const noexcept {
    return _raw_set;
}

CoreSet
CoreSet::build(uint32_t const nb_core) noexcept {
    CoreIdSet set;
    // `CoreId` is uint16_t while the parameter is uint32_t: an `nb_core` above 65535 would wrap the
    // counter back to 0 and spin forever (the set stops growing, so nothing else would ever signal
    // it). No real machine reaches that, but this takes a uint32_t from the caller, so clamp to what
    // a CoreId can actually represent rather than rely on the argument being sane.
    const uint32_t bound = std::min<uint32_t>(nb_core, std::numeric_limits<CoreId>::max() + 1u);
    for (uint32_t i = 0; i < bound; ++i)
        set.insert(static_cast<CoreId>(i));
    return CoreSet{set};
}
} // namespace qb