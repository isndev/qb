/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#include <algorithm>
#include <numeric>
#include <qb/core/CoreSet.h>

namespace qb {

CoreSet::CoreSet(CoreIdSet const &set) noexcept
    : _raw_set(set)
    , _nb_core(set.size())
    , _size([&set]() {
        // Trouver le maximum des IDs dans le bitset
        for (int i = MaxCores - 1; i >= 0; --i) {
            if (set.contains(i)) {
                return static_cast<std::size_t>(i + 1u);
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
    return _set.at(id);
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