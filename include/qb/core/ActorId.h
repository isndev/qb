/**
 * @file qb/core/ActorId.h
 * @brief Actor and core identification for the QB Actor Framework
 *
 * This file defines the core identification types and the ActorId class which is used
 * for uniquely identifying actors within the QB Actor Framework. It provides types for
 * core IDs, service IDs, and actor IDs, as well as utilities for set operations on
 * collections of core IDs.
 *
 * The ActorId is a compound identifier that includes both the core ID where an actor
 * is located and a service ID that uniquely identifies the actor within that core.
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

#ifndef QB_ACTORID_H
#define QB_ACTORID_H
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>
// include from qb
#include <qb/io.h>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/build_macros.h>
#include <qb/utility/prefix.h>

namespace qb {

using CoreId    = uint16_t;
using ServiceId = uint16_t;
using TypeId    = uint16_t;
using EventId   = TypeId;

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
     */
    CoreIdBitSet() = default;

    /**
     * @brief Constructor from a set of core IDs
     * @param coreIds Set of core IDs to include
     */
    explicit CoreIdBitSet(const qb::unordered_set<CoreId> &coreIds) {
        for (const auto id : coreIds) {
            _bits.set(id);
        }
    }

    /**
     * @brief Constructor from an initializer list
     * @param ids List of core IDs to include
     */
    CoreIdBitSet(std::initializer_list<CoreId> ids) {
        for (const auto id : ids) {
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

        for (size_t i = 0; i < MaxCores; ++i) {
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

        for (size_t i = 0; i < MaxCores; ++i) {
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

        iterator(const CoreIdBitSet &set, size_t pos)
            : _set(set)
            , _pos(pos) {
            advance();
        }

        CoreId
        operator*() const {
            return static_cast<CoreId>(_pos);
        }

        iterator &
        operator++() {
            ++_pos;
            advance();
            return *this;
        }

        iterator
        operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool
        operator==(const iterator &other) const {
            return &_set == &other._set && _pos == other._pos;
        }

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
using CoreIdSet = CoreIdBitSet;

/*!
 * @class ActorId core/ActorId.h qb/actorid.h
 * @ingroup Core
 * @brief Unique identifier for actors
 * @details
 * ActorId combines a service/actor identifier with a core identifier to form a unique
 * identifier for an actor within the actor system. It provides methods for creating,
 * comparing, and validating actor IDs.
 */
class ActorId {
    template <typename T>
    friend struct std::hash;

    friend class CoreInitializer;
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Actor;
    friend class Service;

private:
    ServiceId _service_id;
    CoreId    _core_id;

protected:
    ActorId(ServiceId id, CoreId index) noexcept;

public:
    static constexpr uint32_t  NotFound     = 0;
    static constexpr ServiceId BroadcastSid = (std::numeric_limits<ServiceId>::max)();

    /*!
     * ActorId() == ActorId::NotFound
     */
    ActorId() noexcept;
    /*!
     * @private
     * internal function
     */
    ActorId(uint32_t id) noexcept;

    /**
     * @brief Conversion operator to uint32_t
     * @return The ActorId as a 32-bit unsigned integer
     */
    [[nodiscard]] operator uint32_t() const noexcept;

    /*!
     * @brief Get the service identifier component of this ActorId
     * @return Service identifier
     */
    [[nodiscard]] ServiceId sid() const noexcept;

    /*!
     * @brief Get the core identifier component of this ActorId
     * @return VirtualCore identifier
     */
    [[nodiscard]] CoreId index() const noexcept;

    /*!
     * @brief Check if this ActorId represents a broadcast identifier
     * @return true if ActorId is a Core broadcast id, false otherwise
     */
    [[nodiscard]] bool is_broadcast() const noexcept;

    /*!
     * @brief Check if this ActorId is valid (not NotFound)
     * @return true if ActorId is valid, false otherwise
     */
    [[nodiscard]] bool is_valid() const noexcept;
};

class BroadcastId : public ActorId {
public:
    BroadcastId() = delete;
    explicit BroadcastId(uint32_t const core_id) noexcept
        : ActorId(BroadcastSid, static_cast<CoreId>(core_id)) {}
};

using ActorIdList   = std::vector<ActorId>;
using ActorIdSet    = std::unordered_set<ActorId>;
using core_id       = CoreId;
using service_id    = ServiceId;
using actor_id      = ActorId;
using broadcast_id  = BroadcastId;
using actor_id_list = ActorIdList;
using actor_is_set  = ActorIdSet;
using core_id_set   = CoreIdSet;
#ifdef QB_LOGGER
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::ActorId const &id);
#endif
} // namespace qb

namespace std {
template <>
struct hash<qb::ActorId> {
    std::size_t
    operator()(qb::ActorId const &val) const noexcept {
        return static_cast<uint32_t>(val);
    }
};
} // namespace std

#endif // QB_ACTORID_H
