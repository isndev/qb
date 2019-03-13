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

#ifndef QB_CORESET_H
# define QB_CORESET_H
# include <cstdint>
# include <vector>
# include <unordered_set>

namespace qb {

    /*!
     * @class CoreSet core/CoreSet.h qb/coreset.h
     * @ingroup Core
     * @brief Main initializer
     */
    class CoreSet {
        friend class Main;

        const std::unordered_set<uint8_t>  _raw_set;
        const std::size_t       _nb_core;
        std::vector<uint8_t>    _set;
        std::size_t             _size;

        uint8_t resolve(std::size_t const id) const;
        std::size_t getSize() const;
        std::size_t getNbCore() const;

    public:
        CoreSet() = delete;
        CoreSet(CoreSet const &) = default;
        explicit CoreSet(std::unordered_set<uint8_t> const &set);

        static CoreSet build(uint32_t const nb_core = std::thread::hardware_concurrency());
    };

} // namespace qb

#endif //QB_CORESET_H
