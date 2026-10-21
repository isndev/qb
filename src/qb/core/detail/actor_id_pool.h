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

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <qb/core/ActorId.h>
#include <qb/system/allocator/region.h>
#include <qb/utility/branch_hints.h>

namespace qb {
class VirtualCore;
}

namespace qb::detail {

// Engine-run incarnation metadata. Ordinary slots and service tags have independent banks;
// neither a service reservation nor a lookup materializes the 24-bit address space.
class ActorIdPool {
    friend class qb::VirtualCore;

    struct Entry {
        std::uint32_t state      = 0;
        std::uint32_t generation = 1;
    };
    static_assert(sizeof(Entry) == 8);
    static_assert(offsetof(Entry, state) == 0 && offsetof(Entry, generation) == 4);
    static_assert(std::is_trivially_copyable_v<Entry>);
    static_assert(sizeof(ActorId) == 8 && std::is_trivially_copyable_v<ActorId>);
    static constexpr std::uint32_t Reserved       = 0xffffffffu;
    static constexpr std::uint32_t Live           = 0xfffffffeu;
    static constexpr std::uint32_t Retiring       = 0xfffffffdu;
    static constexpr std::uint32_t Retired        = 0xfffffffcu;
    static constexpr std::uint64_t NextGeneration = std::uint64_t{1} << 32u;

    // An ordinary free row stores the next FULL identity, not its own generation:
    // that generation travels in the incoming link (or _free_head). A free link's
    // low word is core8|ordinary-slot24, or zero, always below every state sentinel.
    // Public transitions therefore reject a free row even if its next generation
    // happens to match an old identity for this row.
    static_assert(((static_cast<std::uint32_t>(MaxCores - 1) << 24u) | ActorId::MaxOrdinarySlot) < Retired);

    static QB_MSVC_FORCEINLINE std::uint64_t
                               free_link(Entry const &item) noexcept {
        if constexpr (std::endian::native == std::endian::little)
            return std::bit_cast<std::uint64_t>(item);
        else
            return (static_cast<std::uint64_t>(item.generation) << 32u) | item.state;
    }

    static QB_MSVC_FORCEINLINE void
    set_free_link(Entry &item, std::uint64_t const next) noexcept {
        if constexpr (std::endian::native == std::endian::little)
            item = std::bit_cast<Entry>(next);
        else {
            item.state      = static_cast<std::uint32_t>(next);
            item.generation = static_cast<std::uint32_t>(next >> 32u);
        }
    }

    CoreId                          _core;
    ActorSlotId                     _ordinary_limit;
    std::uint32_t                   _generation_limit;
    std::uint64_t                   _free_head     = 0; ///< next ordinary incarnation, already advanced and canonical
    std::uint64_t                   _core_bits     = 0; ///< `_core << 24`: an ordinary id's core field, precomputed
    std::size_t                     _ordinary_rows = 0; ///< `_ordinary.size()`, kept beside it for entry()'s bound
    allocator::region_vector<Entry> _ordinary;
    allocator::region_vector<Entry> _services;

    // The row of exactly this incarnation, or nullptr. An ordinary id -- every admission and removal asks
    // several times -- is answered inline: one bounds test, then the complete identity rebuilt from the row (its
    // generation, this core, the slot) compared with the id, which checks generation and core at once. Every
    // caller also checks the state: a free row's generation belongs to its next link, not to this row.
    // A pool whose core the codec cannot carry has no row. The service band, the broadcast slot and the band
    // marker take the full decode out of line. The answers are the full decode's for every id. Out of line, the
    // decode alone was 36 instructions per call on MSVC 19.51, five calls per actor lifetime (single-stepped,
    // Huly QB-970 perf lot).
    [[nodiscard]] QB_MSVC_FORCEINLINE Entry *
    entry(ActorId const id) noexcept {
        const std::uint64_t raw  = id.raw64();
        const std::size_t   slot = static_cast<std::size_t>(raw & ActorId::BroadcastSid);
        if (slot > ActorId::ServiceBand) [[unlikely]]
            return service_entry(id);
        // One bound covers the rest: the band marker itself (0xFF0000) lies past every ordinary row (the bank never
        // exceeds MaxOrdinarySlot + 1), and row zero -- generation 1, never handed out, state 0 -- can only match a
        // slot-0 value, which no ActorId holds (NotFound is all zero); were one forged, row zero's state refuses
        // every transition below.
        if (slot >= _ordinary_rows)
            return nullptr;
        Entry &item = _ordinary[slot];
        return raw == ((static_cast<std::uint64_t>(item.generation) << 32u) | _core_bits | slot) ? &item : nullptr;
    }
    [[nodiscard]] QB_MSVC_FORCEINLINE Entry const *
    entry(ActorId const id) const noexcept {
        return const_cast<ActorIdPool *>(this)->entry(id);
    }
    [[nodiscard]] QB_NOINLINE Entry  *service_entry(ActorId id) noexcept;
    QB_NOINLINE void                  release_slow(ActorId id) noexcept;
    [[nodiscard]] QB_NOINLINE ActorId reserve_ordinary_grow();

    // These transitions belong to VirtualCore's ownership protocol, not to callers supplying
    // an arbitrary identity. Keep the checked public transitions for that boundary.
    // A slot number survives nested admissions; an Entry pointer does not survive bank growth.
    QB_MSVC_FORCEINLINE void
    commit_reserved(bool const service, std::size_t const slot) noexcept {
        auto &bank = service ? _services : _ordinary;
        assert(slot < bank.size() && bank[slot].state == Reserved);
        bank[slot].state = Live;
    }

    QB_MSVC_FORCEINLINE void
    begin_retirement(bool const service, std::size_t const slot) noexcept {
        auto &bank = service ? _services : _ordinary;
        assert(slot < bank.size() && bank[slot].state == Live);
        bank[slot].state = Retiring;
    }

    QB_MSVC_FORCEINLINE void
    finish_retirement(ActorId const id, bool const service, std::size_t const slot) noexcept {
        auto &bank = service ? _services : _ordinary;
        assert(slot < bank.size() && bank[slot].state == Retiring);
        Entry &item = bank[slot]; // reacquired after the actor's reentrant destructor
        assert(entry(id) == &item);
        if (item.generation >= _generation_limit) [[unlikely]] {
            item.state = Retired;
            return;
        }
        if (service) {
            ++item.generation;
            item.state = 0;
        } else {
            set_free_link(item, _free_head);
            _free_head = id.raw64() + NextGeneration;
        }
    }

public:
    // Reduced limits exercise exhaustion deterministically; production uses the codec ceilings.
    explicit ActorIdPool(CoreId core, ActorSlotId ordinary_limit = ActorId::MaxOrdinarySlot,
                         std::uint32_t generation_limit = ActorId::MaxGeneration) noexcept;
    [[nodiscard]] ActorIdPool take() noexcept;
    // A freed incarnation first, inline. Every incoming link was formed by advancing a checked identity below
    // the generation ceiling; it is canonical and names a row in this pool. No public codec check or recomposition
    // is needed here. Opening a new row (and the exhaustion test) is out of line.
    [[nodiscard]] QB_MSVC_FORCEINLINE ActorId
    reserve_ordinary() {
        if (const std::uint64_t raw = _free_head; raw != 0) [[likely]] {
            Entry &item = _ordinary[static_cast<ActorSlotId>(raw & ActorId::BroadcastSid)];
            _free_head  = free_link(item);
            item        = Entry{Reserved, static_cast<std::uint32_t>(raw >> 32u)};
            return std::bit_cast<ActorId>(raw); // a canonical identity produced only by this pool's release
        }
        return reserve_ordinary_grow();
    }
    [[nodiscard]] ActorId reserve_service(ServiceId tag);
    // Return an incarnation's slot: its generation advances (or the row retires at the generation limit) and an
    // ordinary slot goes back on the free list. The state test is one compare: Retiring, Live and Reserved are the
    // three highest sentinels. A service tag, or an id this pool does not hold, takes the out-of-line path.
    QB_MSVC_FORCEINLINE void
    release(ActorId const id) noexcept {
        const auto slot = static_cast<ActorSlotId>(id.raw64() & ActorId::BroadcastSid);
        Entry     *item = slot > ActorId::ServiceBand ? nullptr : entry(id);
        if (item == nullptr) [[unlikely]] {
            release_slow(id);
            return;
        }
        if (item->state < Retiring)
            return;
        if (item->generation >= _generation_limit) [[unlikely]] {
            item->state = Retired;
            return;
        }
        set_free_link(*item, _free_head);
        _free_head = id.raw64() + NextGeneration;
    }
    // The three state checks inline, over the inline row lookup: Reserved -> Live at admission, Live -> Retiring at
    // removal, and the reservation test both ends of an admission ask. Forced on MSVC: 19.51 kept owns_reservation()
    // out of line with the lookup inlined into it (31 instructions and a call, twice per admission).
    [[nodiscard]] QB_MSVC_FORCEINLINE bool
    mark_live(ActorId const id) noexcept {
        Entry *const item = entry(id);
        if (item == nullptr || item->state != Reserved)
            return false;
        item->state = Live;
        return true;
    }
    [[nodiscard]] QB_MSVC_FORCEINLINE bool
    mark_retiring(ActorId const id) noexcept {
        Entry *const item = entry(id);
        if (item == nullptr || item->state != Live)
            return false;
        item->state = Retiring;
        return true;
    }
    [[nodiscard]] QB_MSVC_FORCEINLINE bool
    owns_reservation(ActorId const id) const noexcept {
        Entry const *const item = entry(id);
        return item != nullptr && item->state == Reserved;
    }
    void               clear_reservations() noexcept;
    [[nodiscard]] bool empty() const noexcept;
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
