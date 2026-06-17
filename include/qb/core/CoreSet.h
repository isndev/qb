/**
 * @file qb/core/CoreSet.h
 * @brief Internal core-set mapping used by the QB engine for inter-core routing.
 *
 * Defines `qb::CoreSet`, a compact bidirectional mapping between logical `CoreId`
 * values and their dense indices used internally by `SharedCoreCommunication` and
 * `VirtualCore` to route events across worker threads.
 *
 * ### Relationship to `CoreIdSet` / `CoreIdBitSet`
 * `qb::CoreIdSet` (alias of `qb::CoreIdBitSet`, defined in `ActorId.h`) is the
 * **user-facing** collection for specifying CPU affinity and checking reachability.
 * `qb::CoreSet` is the **internal** engine object that wraps a `CoreIdSet` and
 * adds the resolved-index lookup table needed for O(1) mailbox addressing.
 *
 * Application code rarely constructs `CoreSet` directly; use `CoreIdSet` when
 * configuring `CoreInitializer::setAffinity()` or querying `Actor::getCoreSet()`.
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

#ifndef QB_CORE_SET_H
#define QB_CORE_SET_H
#include <cstdint>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <vector>

// include from qb
#include "ActorId.h"

namespace qb {

/*!
 * @class CoreSet core/CoreSet.h qb/coreset.h
 * @ingroup Engine
 * @brief Immutable, resolved mapping from a `CoreIdSet` to compact mailbox indices.
 *
 * @details
 * Internally the QB engine needs to translate a logical `CoreId` (an arbitrary
 * `uint16_t` chosen by the user) to a dense, zero-based index that can address
 * into the `SharedCoreCommunication::_mail_boxes` vector in O(1) time.
 * `CoreSet` pre-computes and stores that mapping at construction time.
 *
 * The class is intentionally immutable after construction — the core layout of a
 * running engine does not change.
 *
 * ### Usage by application code
 * Application code typically encounters `CoreSet` only through
 * `Actor::getCoreSet()`, which returns a const reference to the `CoreSet`
 * embedded in the actor's `VirtualCore`. The returned set describes the full set
 * of cores the engine was started with — useful, for example, when an actor wants
 * to broadcast an event to *every* core by iterating its raw ids.
 *
 * ```cpp
 * // Iterate all engine cores and push an event to a service on each
 * for (qb::CoreId cid : getCoreSet().raw()) {
 *     auto svc_id = qb::Actor::getServiceId<MyServiceTag>(cid);
 *     if (svc_id.is_valid())
 *         push<NotifyEvent>(svc_id);
 * }
 * ```
 *
 * @note This class is non-copyable and non-default-constructible by design —
 *       callers must construct it from an explicit `CoreIdSet`.
 */
class CoreSet {
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Main;

    const CoreIdSet               _raw_set;
    const std::size_t             _nb_core;
    const std::size_t             _size;
    std::array<uint8_t, MaxCores> _set{};

public:
    CoreSet() = delete;

    /*!
     * @brief Construct a `CoreSet` from an explicit set of core identifiers.
     * @param set The `CoreIdSet` enumerating each logical `CoreId` that belongs
     *            to this set. The set must not be empty; the engine asserts this
     *            at startup.
     */
    explicit CoreSet(CoreIdSet const &set) noexcept;

    /*!
     * @brief Factory helper — build a `CoreSet` with the first `nb_core` cores.
     * @param nb_core Number of sequential core IDs to include (`0, 1, ..., nb_core-1`).
     *               Defaults to `std::thread::hardware_concurrency()`.
     * @return A newly constructed `CoreSet` covering `{0, 1, …, nb_core-1}`.
     * @details Convenience factory for the common case where cores are allocated
     *          starting from index 0 up to the hardware thread count.
     */
    [[nodiscard]] static CoreSet build(uint32_t nb_core = std::thread::hardware_concurrency()) noexcept;

    /*!
     * @brief Resolve a logical `CoreId` to its dense mailbox index.
     * @param id Logical `CoreId` to look up.
     * @return Zero-based index of `id` within this set. Returns an unspecified
     *         value if `id` is not a member of the set — callers should verify
     *         membership via `raw()` before calling `resolve()`.
     */
    [[nodiscard]] CoreId resolve(std::size_t id) const noexcept;

    /*!
     * @brief Access the underlying set of logical `CoreId` values.
     * @return Const reference to the `CoreIdSet` this object was built from.
     *         Suitable for range-based iteration over all member core IDs.
     */
    [[nodiscard]] const CoreIdSet &raw() const noexcept;

    /*!
     * @brief Total capacity of the internal index table (not the same as `getNbCore()`).
     * @return The size of the internal look-up array (`MaxCores`).
     * @note In most cases `getNbCore()` is more useful; this method is provided for
     *       low-level diagnostics.
     */
    [[nodiscard]] uint32_t getSize() const noexcept;

    /*!
     * @brief Number of `CoreId` values in this set.
     * @return Count of distinct `CoreId`s this `CoreSet` covers.
     */
    [[nodiscard]] uint32_t getNbCore() const noexcept;
};

} // namespace qb
#endif // QB_CORESET_H
