/**
 * @file qb/core/detail/actor_id_pool.h
 * @brief Engine-run actor incarnation allocation metadata.
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

#ifndef QB_CORE_DETAIL_ACTOR_ID_POOL_H
#define QB_CORE_DETAIL_ACTOR_ID_POOL_H

#include <cstddef>
#include <cstdint>
#include <qb/core/ActorId.h>
#include <qb/system/allocator/region.h>

namespace qb::detail {

// Engine-run incarnation metadata. Ordinary slots and service tags have independent banks;
// neither a service reservation nor a lookup materializes the 24-bit address space.
class ActorIdPool {
    struct Entry {
        std::uint32_t generation = 1;
        std::uint32_t state      = 0; // free-next while free, a sentinel otherwise
    };
    static_assert(sizeof(Entry) == 8);
    static constexpr std::uint32_t Reserved = 0xffffffffu;
    static constexpr std::uint32_t Live     = 0xfffffffeu;
    static constexpr std::uint32_t Retiring = 0xfffffffdu;
    static constexpr std::uint32_t Retired  = 0xfffffffcu;

    CoreId                          _core;
    ActorSlotId                     _ordinary_limit;
    std::uint32_t                   _generation_limit;
    ActorSlotId                     _free_head = 0;
    allocator::region_vector<Entry> _ordinary;
    allocator::region_vector<Entry> _services;

    [[nodiscard]] Entry       *entry(ActorId id) noexcept;
    [[nodiscard]] Entry const *entry(ActorId id) const noexcept;

public:
    // Reduced limits exercise exhaustion deterministically; production uses the codec ceilings.
    explicit ActorIdPool(CoreId core, ActorSlotId ordinary_limit = ActorId::MaxOrdinarySlot,
                         std::uint32_t generation_limit = ActorId::MaxGeneration) noexcept;
    [[nodiscard]] ActorIdPool take() noexcept;
    [[nodiscard]] ActorId     reserve_ordinary();
    [[nodiscard]] ActorId     reserve_service(ServiceId tag);
    void                      release(ActorId id) noexcept;
    [[nodiscard]] bool        mark_live(ActorId id) noexcept;
    [[nodiscard]] bool        mark_retiring(ActorId id) noexcept;
    [[nodiscard]] bool        owns_reservation(ActorId id) const noexcept;
    void                      clear_reservations() noexcept;
    [[nodiscard]] bool        empty() const noexcept;
    /// Allocated metadata capacity in bytes, including bank sentinel rows and unused capacity.
    [[nodiscard]] std::size_t metadata_bytes() const noexcept;
    /// Indexed metadata rows, excluding each bank's row zero; service-tag holes are included.
    [[nodiscard]] std::size_t slot_count() const noexcept;
    /// Ordinary-bank size including the invalid row zero when the bank is allocated.
    [[nodiscard]] std::size_t ordinary_slot_count() const noexcept;
    /// Service-bank size including the invalid row zero when the bank is allocated.
    [[nodiscard]] std::size_t service_slot_count() const noexcept;
};

} // namespace qb::detail

#endif // QB_CORE_DETAIL_ACTOR_ID_POOL_H
