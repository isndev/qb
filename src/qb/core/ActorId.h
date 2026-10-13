/**
 * @file qb/core/ActorId.h
 * @brief Actor and core identification for the QB Actor Framework
 *
 * This file defines the core identification types and the ActorId class which is used
 * for uniquely identifying actors within the QB Actor Framework. It provides types for
 * core IDs, service IDs, and actor IDs, as well as utilities for set operations on
 * collections of core IDs.
 *
 * ActorId combines a core, an actor slot and an incarnation generation. Logical
 * service addresses use a separate slot band and resolve to the current instance.
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

#ifndef QB_ACTORID_H
#define QB_ACTORID_H
#include <algorithm>
#include <compare>
#include <bitset>
#include <cstdint>
#include <bit>
#include <limits>
#include <iosfwd>
#include <unordered_set>
#include <vector>
// include from qb
#include <qb/io.h>
#include <qb/system/container/unordered_set.h>
#include <qb/system/event/router.h> // router::dense_index<ActorId>
#include <qb/utility/build_macros.h>
#include <qb/utility/prefix.h>

namespace qb {

/**
 * @typedef CoreId
 * @brief Type definition for core identifiers
 * @details A 16-bit unsigned integer that uniquely identifies a core in the system
 * @ingroup Core
 */
using CoreId = uint16_t;

/**
 * @typedef ServiceId
 * @brief Type definition for service identifiers
 * @details A 16-bit unsigned integer that uniquely identifies a service within a core
 * @ingroup Core
 */
using ServiceId = uint16_t;
/** @brief Unsigned storage for a 24-bit ordinary or service actor slot. */
using ActorSlotId = uint32_t;

/**
 * @typedef TypeId
 * @brief Type definition for type identifiers
 * @details A 16-bit unsigned integer that uniquely identifies types in the type system
 * @ingroup Core
 */
using TypeId = uint16_t;

/**
 * @typedef EventId
 * @brief Type definition for event identifiers
 * @details Equivalent to TypeId, used for identifying event types
 * @ingroup Core
 */
using EventId = TypeId;

/**
 * @brief Maximum number of cores supported in a system
 */
constexpr size_t MaxCores = 256;

/**
 * @class CoreIdBitSet
 * @brief Efficient representation of a set of core IDs using a bitset
 * @details
 * This class provides bitset-based storage for core IDs, which is more memory
 * efficient and provides faster set operations than unordered_set.
 */
class CoreIdBitSet {
private:
    std::bitset<MaxCores> _bits;

public:
    /**
     * @brief Default constructor - creates an empty set
     * @ingroup Actor
     */
    CoreIdBitSet() = default;

    /**
     * @brief Constructor from a set of core IDs
     * @param coreIds Set of core IDs to include
     * @ingroup Actor
     */
    explicit CoreIdBitSet(const qb::unordered_set<CoreId> &coreIds) {
        // Silently skip out-of-range CoreIds (notably the public sentinel
        // `qb::NoAffinity = CoreId::max()`) so that `CoreIdSet{NoAffinity}`
        // evaluates to "no pinning" rather than throwing `std::out_of_range`
        // from `std::bitset::set` (finding 2.11).
        for (const auto id : coreIds) {
            if (id < static_cast<CoreId>(MaxCores))
                _bits.set(id);
        }
    }

    /**
     * @brief Constructor from an initializer list
     * @param ids List of core IDs to include
     * @ingroup Actor
     */
    CoreIdBitSet(std::initializer_list<CoreId> ids) {
        // Out-of-range ids (notably `qb::NoAffinity`) are silently skipped
        // so the constructor mirrors the runtime filter in
        // `VirtualCore::__init__` (finding 2.11).
        for (const auto id : ids) {
            if (id < static_cast<CoreId>(MaxCores))
                _bits.set(id);
        }
    }

    /**
     * @brief Get the raw bitset
     * @return Reference to the underlying bitset
     */
    [[nodiscard]] const std::bitset<MaxCores> &
    bits() const noexcept {
        return _bits;
    }

    /**
     * @brief Check if a core ID is in the set
     * @param id Core ID to check
     * @return true if the ID is in the set, false otherwise
     */
    [[nodiscard]] bool
    contains(CoreId id) const noexcept {
        return id < MaxCores && _bits.test(id);
    }

    /**
     * @brief Add a core ID to the set
     * @param id Core ID to add
     */
    void
    insert(CoreId id) noexcept {
        if (id < MaxCores) {
            _bits.set(id);
        }
    }

    /**
     * @brief Add a core ID to the set (emplace version)
     * @param id Core ID to add
     */
    void
    emplace(CoreId id) noexcept {
        insert(id);
    }

    /**
     * @brief Remove a core ID from the set
     * @param id Core ID to remove
     */
    void
    remove(CoreId id) noexcept {
        if (id < MaxCores) {
            _bits.reset(id);
        }
    }

    /**
     * @brief Clear all core IDs from the set
     */
    void
    clear() noexcept {
        _bits.reset();
    }

    /**
     * @brief Check if the set is empty
     * @return true if the set is empty, false otherwise
     */
    [[nodiscard]] bool
    empty() const noexcept {
        return _bits.none();
    }

    /**
     * @brief Get the number of core IDs in the set
     * @return Count of core IDs
     */
    [[nodiscard]] size_t
    size() const noexcept {
        return _bits.count();
    }

    /**
     * @brief Convert the set to a vector of core IDs
     * @return Vector containing all core IDs in the set
     */
    [[nodiscard]] std::vector<CoreId>
    to_vector() const {
        std::vector<CoreId> result;
        result.reserve(_bits.count());

        for (std::size_t i = 0; i < MaxCores; ++i) {
            if (_bits.test(i)) {
                result.push_back(static_cast<CoreId>(i));
            }
        }

        return result;
    }

    /**
     * @brief Get an unordered_set of the core IDs
     * @return Unordered set containing all core IDs
     */
    [[nodiscard]] qb::unordered_set<CoreId>
    to_unordered_set() const {
        qb::unordered_set<CoreId> result;

        for (std::size_t i = 0; i < MaxCores; ++i) {
            if (_bits.test(i)) {
                result.insert(static_cast<CoreId>(i));
            }
        }

        return result;
    }

    /**
     * @brief Get a reference to the raw set for internal use
     * @return Reference to the unordered_set for internal use
     */
    [[nodiscard]] qb::unordered_set<CoreId>
    raw() const {
        return to_unordered_set();
    }

    // Iteration support
    /**
     * @class iterator
     * @brief Iterator for traversing set bits in a CoreIdBitSet
     * @details
     * Provides forward iterator functionality for efficiently iterating over
     * the core IDs stored in a CoreIdBitSet. The iterator automatically
     * advances to the next set bit and provides standard iterator operations.
     * @ingroup Core
     */
    class iterator {
    private:
        const CoreIdBitSet &_set;
        size_t              _pos;

        // Advance to the next set bit
        void
        advance() {
            while (_pos < MaxCores && !_set._bits.test(_pos)) {
                ++_pos;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = CoreId;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const CoreId *;
        using reference         = const CoreId &;

        /**
         * @brief Constructs an iterator for a CoreIdBitSet
         * @param set Reference to the CoreIdBitSet to iterate
         * @param pos Starting position for iteration
         */
        iterator(const CoreIdBitSet &set, size_t pos)
            : _set(set)
            , _pos(pos) {
            advance();
        }

        /**
         * @brief Dereference operator
         * @return The current CoreId value
         */
        CoreId
        operator*() const {
            return static_cast<CoreId>(_pos);
        }

        /**
         * @brief Pre-increment operator
         * @return Reference to this iterator after advancement
         */
        iterator &
        operator++() {
            ++_pos;
            advance();
            return *this;
        }

        /**
         * @brief Post-increment operator
         * @return Copy of iterator before advancement
         */
        iterator
        operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        /**
         * @brief Equality comparison operator
         * @param other Iterator to compare with
         * @return True if iterators point to the same position
         */
        bool
        operator==(const iterator &other) const {
            return &_set == &other._set && _pos == other._pos;
        }

        /**
         * @brief Inequality comparison operator
         * @param other Iterator to compare with
         * @return True if iterators point to different positions
         */
        bool
        operator!=(const iterator &other) const {
            return !(*this == other);
        }
    };

    /**
     * @brief Get an iterator to the beginning of the set
     * @return Iterator to the first core ID
     */
    [[nodiscard]] iterator
    begin() const {
        return iterator(*this, 0);
    }

    /**
     * @brief Get an iterator to the end of the set
     * @return Iterator representing the end of the set
     */
    [[nodiscard]] iterator
    end() const {
        return iterator(*this, MaxCores);
    }
};

// Define CoreIdSet as our new efficient implementation
/**
 * @typedef CoreIdSet
 * @brief Efficient set implementation for storing CoreId values
 * @details
 * Uses the CoreIdBitSet implementation for memory-efficient and
 * high-performance storage and manipulation of core identifier sets.
 * @ingroup Core
 */
using CoreIdSet = CoreIdBitSet;

/*!
 * @class ActorId
 * @ingroup Actor
 * @brief Unique identifier for actors
 * @details
 * ActorId stores generation32, core8 and slot24 in a 64-bit value. Ordinary
 * instances use slots 1..0xFEFFFF and a nonzero generation. The 0xFFxxxx band
 * holds service tags; generation zero names a logical service address, while
 * a nonzero generation identifies one service incarnation. Slot 0xFFFFFF with
 * generation zero is the per-core broadcast address. Equality compares every bit.
 * Identities are scoped to an engine run, not a process-global or durable epoch.
 */
class ActorId {
    template <typename>
    friend struct std::hash;
    template <typename>
    friend struct qb::router::dense_index;
    friend class CoreInitializer;
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Actor;
    friend class Service;

    std::uint64_t _value{0};

    static constexpr std::uint64_t
    pack(ActorSlotId slot, CoreId core, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32u) | (static_cast<std::uint64_t>(core) << 24u) | slot;
    }

protected:
    // Existing two-component internal call sites name a logical service, not an incarnation.
    ActorId(ServiceId tag, CoreId core) noexcept
        : ActorId(service_address(tag, core)) {}

public:
    static constexpr std::uint64_t NotFound        = 0;
    static constexpr ActorSlotId   BroadcastSid    = 0x00ffffffu;
    static constexpr ActorSlotId   ServiceBand     = 0x00ff0000u;
    static constexpr ActorSlotId   MaxOrdinarySlot = ServiceBand - 1u;
    static constexpr std::uint32_t MaxGeneration   = (std::numeric_limits<std::uint32_t>::max)();

    constexpr ActorId() noexcept = default;
    // No lossy 32-bit round trip is supported by this experimental codec.
    ActorId(std::uint32_t) = delete;
    constexpr ActorId(std::uint64_t raw) noexcept
        : _value(raw) {
        if (!is_valid())
            _value = 0;
    }

    [[nodiscard]] static constexpr ActorId
    from_raw(std::uint64_t raw) noexcept {
        return ActorId(raw);
    }
    [[nodiscard]] static constexpr ActorId
    from_parts(ActorSlotId slot, CoreId core, std::uint32_t generation) noexcept {
        if (core >= MaxCores || slot == 0 || slot > BroadcastSid)
            return {};
        return ActorId(pack(slot, core, generation));
    }
    [[nodiscard]] static constexpr ActorId
    service_address(ServiceId tag, CoreId core) noexcept {
        if (tag == 0 || tag == (std::numeric_limits<ServiceId>::max)())
            return {};
        return from_parts(ServiceBand | tag, core, 0);
    }
    [[nodiscard]] static constexpr ActorId
    service_instance(ServiceId tag, CoreId core, std::uint32_t generation) noexcept {
        if (generation == 0 || tag == 0 || tag == (std::numeric_limits<ServiceId>::max)())
            return {};
        return from_parts(ServiceBand | tag, core, generation);
    }
    [[nodiscard]] static constexpr ActorId
    ordinary(ActorSlotId slot, CoreId core, std::uint32_t generation) noexcept {
        if (slot > MaxOrdinarySlot || generation == 0)
            return {};
        return from_parts(slot, core, generation);
    }
    [[nodiscard]] constexpr std::uint64_t
    raw64() const noexcept {
        return _value;
    }
    [[nodiscard]] constexpr ActorSlotId
    slot() const noexcept {
        return static_cast<ActorSlotId>(_value & BroadcastSid);
    }
    [[nodiscard]] constexpr ActorSlotId
    sid() const noexcept {
        return slot();
    }
    [[nodiscard]] constexpr CoreId
    index() const noexcept {
        return static_cast<CoreId>((_value >> 24u) & 0xffu);
    }
    [[nodiscard]] constexpr std::uint32_t
    generation() const noexcept {
        return static_cast<std::uint32_t>(_value >> 32u);
    }
    [[nodiscard]] constexpr ServiceId
    service_tag() const noexcept {
        return static_cast<ServiceId>(slot());
    }
    [[nodiscard]] constexpr bool
    is_broadcast() const noexcept {
        return slot() == BroadcastSid && generation() == 0;
    }
    [[nodiscard]] constexpr bool
    is_service() const noexcept {
        return slot() > ServiceBand && slot() < BroadcastSid;
    }
    [[nodiscard]] constexpr bool
    is_service_address() const noexcept {
        return is_service() && generation() == 0;
    }
    [[nodiscard]] constexpr bool
    is_service_instance() const noexcept {
        return is_service() && generation() != 0;
    }
    [[nodiscard]] constexpr bool
    is_ordinary() const noexcept {
        return slot() != 0 && slot() <= MaxOrdinarySlot && generation() != 0;
    }
    [[nodiscard]] constexpr bool
    is_instance() const noexcept {
        return is_ordinary() || is_service_instance();
    }
    [[nodiscard]] constexpr bool
    is_valid() const noexcept {
        return is_instance() || is_service_address() || is_broadcast();
    }

    friend constexpr bool operator==(ActorId, ActorId) noexcept  = default;
    friend constexpr auto operator<=>(ActorId, ActorId) noexcept = default;
};

/**
 * @class BroadcastId
 * @brief Specialized ActorId for broadcasting messages to all actors on a core
 * @details
 * BroadcastId is used to send messages to all actors on a specific core.
 * It uses the special BroadcastSid value as the service ID to indicate
 * that the message should be delivered to all actors on the specified core.
 * @ingroup Actor
 */
class BroadcastId : public ActorId {
public:
    BroadcastId() = delete;
    /**
     * @brief Constructor for BroadcastId.
     * @param core_id The core ID to broadcast to.
     * @ingroup Actor
     */
    explicit BroadcastId(uint32_t const core_id) noexcept
        : ActorId(core_id < MaxCores ? ActorId::from_parts(BroadcastSid, static_cast<CoreId>(core_id), 0) : ActorId{}) {}
};

/**
 * @typedef ActorIdList
 * @brief List of actor identifiers
 * @details A vector containing ActorId objects for storing and manipulating collections of actor identifiers
 * @ingroup Core
 */
using ActorIdList = std::vector<ActorId>;

/**
 * @typedef ActorIdSet
 * @brief Set of unique actor identifiers
 * @details An unordered set containing unique ActorId objects with fast lookup capabilities
 * @ingroup Core
 */
using ActorIdSet = std::unordered_set<ActorId>;

/**
 * @typedef core_id
 * @brief Alias for CoreId
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using core_id = CoreId;

/**
 * @typedef service_id
 * @brief Alias for ServiceId
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using service_id = ServiceId;

/**
 * @typedef actor_id
 * @brief Alias for ActorId
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using actor_id = ActorId;

/**
 * @typedef broadcast_id
 * @brief Alias for BroadcastId
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using broadcast_id = BroadcastId;

/**
 * @typedef actor_id_list
 * @brief Alias for ActorIdList
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using actor_id_list = ActorIdList;

/**
 * @typedef actor_is_set
 * @brief Alias for ActorIdSet
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using actor_is_set = ActorIdSet;

/**
 * @typedef core_id_set
 * @brief Alias for CoreIdSet
 * @details Provided for naming consistency with other lowercase aliases
 * @ingroup Core
 */
using core_id_set = CoreIdSet;
std::ostream &operator<<(std::ostream &os, ActorId const &id);
#ifdef QB_WITH_LOGGING
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::ActorId const &id);
#endif
} // namespace qb

namespace qb::router {
/**
 * @brief Dense per-core lookup: ordinary slot in the first bank, service tag in the second.
 * @details Each router belongs to one VirtualCore. The table compares the complete identity,
 *          including generation and core, on every hit. A service's high encoded slot never
 *          sizes the ordinary bank. Logical addresses must be resolved by the receiving core
 *          before dispatch to an instance subscription.
 */
template <>
struct dense_index<qb::ActorId> {
    static constexpr bool        enabled             = true;
    static constexpr std::size_t max_index           = ActorId::MaxOrdinarySlot;
    static constexpr bool        partitioned         = true;
    static constexpr std::size_t secondary_max_index = std::numeric_limits<ServiceId>::max();
    [[nodiscard]] static bool
    secondary(qb::ActorId const &id) noexcept {
        return id.is_service() || id.is_broadcast();
    }

    [[nodiscard]] static std::size_t
    of(qb::ActorId const &id) noexcept {
        return static_cast<std::size_t>(secondary(id) ? id.service_tag() : id.slot());
    }
};
} // namespace qb::router

namespace std {
/**
 * @struct hash<qb::ActorId>
 * @brief Specialization of std::hash for qb::ActorId
 * @details
 * This hash specialization allows ActorId objects to be used efficiently
 * in unordered associative containers like std::unordered_set and
 * std::unordered_map. The hash function simply uses the numeric representation
 * of the ActorId.
 */
template <>
struct hash<qb::ActorId> {
    /**
     * @brief Hash function operator for ActorId
     * @param val The ActorId to hash
     * @return Hash value generated from the numeric representation of the ActorId
     */
    std::size_t
    operator()(qb::ActorId const &val) const noexcept {
        const auto bits = val.raw64();
        if constexpr (sizeof(std::size_t) >= sizeof(bits))
            return static_cast<std::size_t>(bits);
        return static_cast<std::size_t>(bits ^ (bits >> 32u));
    }
};
} // namespace std

#endif // QB_ACTORID_H
