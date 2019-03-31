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

#include <qb/core/CoreSet.h>

namespace qb {

    CoreSet::CoreSet(std::unordered_set <CoreId> const &set) noexcept
            : _raw_set(set)
            ,_nb_core(set.size())
            , _size(*std::max_element(set.cbegin(), set.cend()) + 1) {
        _set.resize(_size);
        CoreId idx = 0;
        for (auto id : set)
            _set[id] = idx++;
    }

    CoreId CoreSet::resolve(std::size_t const id) const noexcept {
        return _set[id];
    }

    std::size_t CoreSet::getSize() const noexcept {
        return _size;
    }

    std::size_t CoreSet::getNbCore() const noexcept {
        return _nb_core;
    }

    const std::unordered_set<CoreId> &CoreSet::raw() const noexcept {
        return _raw_set;
    }

    CoreSet CoreSet::build(uint32_t const nb_core) noexcept {
        std::unordered_set<CoreId> set;
        for (CoreId i = 0; i < nb_core; ++i)
            set.insert(i);
        return CoreSet{std::move(set)};
    }
}