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

#ifndef QB_CORE_SET_H
#define QB_CORE_SET_H
#include <cstdint>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/type_traits.h>
#include <vector>

// include from qb
#include "ActorId.h"

namespace qb {

/*!
 * @class CoreSet core/CoreSet.h qb/coreset.h
 * @ingroup Core
 * @brief Manages a set of core identifiers
 * @details
 * CoreSet provides functionality to manage and manipulate sets of core identifiers.
 * It is used to specify which cores should be used for actor execution and
 * communication.
 */
class CoreSet {
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Main;

    const CoreIdSet _raw_set;
    const std::size_t _nb_core;
    const std::size_t _size;
    std::array<uint8_t, MaxCores> _set;

public:
    CoreSet() = delete;

    /*!
     * @brief Construct a CoreSet with a specific set of cores
     * @param set Set of core IDs to include
     */
    explicit CoreSet(CoreIdSet const &set) noexcept;

    /*!
     * @brief Build a CoreSet with a specified number of cores
     * @param nb_core Number of cores to include (defaults to hardware concurrency)
     * @return Newly created CoreSet
     * @details
     * Creates a CoreSet containing sequential core IDs from 0 to nb_core-1.
     * If nb_core is not specified, it uses the number of hardware threads available.
     */
    [[nodiscard]] static CoreSet
    build(uint32_t nb_core = std::thread::hardware_concurrency()) noexcept;

    /*!
     * @brief Resolve a core ID to its index in the set
     * @param id Core ID to resolve
     * @return Index of the core in the set
     */
    [[nodiscard]] CoreId resolve(std::size_t id) const noexcept;

    /*!
     * @brief Get the raw set of core IDs
     * @return Reference to the underlying set of core IDs
     */
    [[nodiscard]] const CoreIdSet& raw() const noexcept;

    /*!
     * @brief Get the size of the core set
     * @return Number of cores in the set
     */
    [[nodiscard]] uint32_t getSize() const noexcept;

    /*!
     * @brief Get the number of cores in the set
     * @return Number of cores
     */
    [[nodiscard]] uint32_t getNbCore() const noexcept;
};

} // namespace qb
#endif // QB_CORESET_H
