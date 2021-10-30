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

#ifndef QB_ACTORID_H
#define QB_ACTORID_H
#include <cstdint>
#include <limits>
#include <unordered_set>
// include from qb
#include <qb/io.h>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/build_macros.h>
#include <qb/utility/prefix.h>

namespace qb {

using CoreId = uint16_t;
using ServiceId = uint16_t;
using TypeId = uint16_t;
using EventId = TypeId;

/*!
 * @class ActorId core/ActorId.h qb/actorid.h
 * @ingroup Core
 * @brief Actor unique identifier
 * @details
 * ActorId is a composition of a Service Index (sid) and VirtualCore Index (index).
 */
class ActorId {
    friend class CoreInitializer;
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Actor;
    friend class Service;

    ServiceId _id;
    CoreId _index;

protected:
    ActorId(ServiceId id, CoreId index) noexcept;

public:
    static constexpr uint32_t NotFound = 0;
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
    ActorId(ActorId const &) = default;

    operator uint32_t() const noexcept;

    /*!
     * @return Service index
     */
    [[nodiscard]] ServiceId sid() const noexcept;
    /*!
     * @return VirtualCore index
     */
    [[nodiscard]] CoreId index() const noexcept;

    /*!
     * @return true if ActorId is a Core broadcast id
     */
    [[nodiscard]] bool is_broadcast() const noexcept;

    /*!
     * @return true if ActorId is valid
     */
    [[nodiscard]] bool is_valid() const noexcept;
};

class BroadcastId : public ActorId {
public:
    BroadcastId() = delete;
    explicit BroadcastId(uint32_t const core_id) noexcept
        : ActorId(BroadcastSid, static_cast<CoreId>(core_id)) {}
};

using CoreIdSet = qb::unordered_set<CoreId>;
using ActorIdList = std::vector<ActorId>;
using ActorIdSet = std::unordered_set<ActorId>;
using core_id = CoreId;
using service_id = ServiceId;
using actor_id = ActorId;
using broadcast_id = BroadcastId;
using actor_id_list = ActorIdList;
using actor_is_set = ActorIdSet;
using core_id_set = CoreIdSet;

qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::ActorId const &id);

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
