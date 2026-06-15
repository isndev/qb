/**
 * @file qb/core/src/CoreSet.cpp
 * @brief Implementation of the CoreSet class for managing core IDs
 *
 * This file contains the implementation of the CoreSet class which manages
 * sets of core IDs and provides functionality for mapping between logical
 * and physical core IDs in the QB Actor Framework.
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

#include <algorithm>
#include <numeric>
#include <qb/core/CoreSet.h>

namespace qb {

CoreSet::CoreSet(CoreIdSet const &set) noexcept
    : _raw_set(set)
    , _nb_core(set.size())
    , _size([&set]() {
        // C++23: Find maximum ID using auto for proper type deduction
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

CoreId
CoreSet::resolve(std::size_t id) const noexcept {
    // `resolve` is noexcept but `_set` is a fixed `std::array<uint8_t, MaxCores>`.
    // A CoreId is a uint16_t, so a caller can legitimately hold an out-of-range
    // value (e.g. BroadcastId(300) or an ActorId with a core_id >= MaxCores).
    // `_set.at(id)` would throw std::out_of_range, and a throw from a noexcept
    // function calls std::terminate — turning a misaddressed event into a hard
    // process abort. Bounds-check instead: out-of-range ids resolve to 0, the
    // same defined-but-unspecified value already returned for an in-range id
    // that is not a member of the set (see the @return contract). The framework
    // then routes-and-drops the misaddressed event rather than crashing.
    return id < MaxCores ? _set[id] : static_cast<CoreId>(0);
}

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
    for (CoreId i = 0; i < nb_core; ++i)
        set.insert(i);
    return CoreSet{set};
}
} // namespace qb