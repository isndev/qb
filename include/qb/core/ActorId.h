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

#ifndef QB_ACTORID_H
# define QB_ACTORID_H
# include <limits>
# include <cstdint>
# include <unordered_set>
 // include from qb
# include <qb/system/container/unordered_set.h>
# include <qb/utility/prefix.h>
# include <qb/utility/build_macros.h>
# include <qb/io.h>

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
        friend class Main;
        friend class VirtualCore;
        friend class Actor;
        friend class Service;

        ServiceId _id;
        CoreId _index;

    protected:
        ActorId(ServiceId const id, CoreId const index) noexcept;
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
        ActorId(uint32_t const id) noexcept;
        ActorId(ActorId const &) noexcept = default;

        operator uint32_t () const noexcept;

        /*!
         * @return Service index
         */
        ServiceId sid() const noexcept;
        /*!
         * @return VirtualCore index
         */
        CoreId index() const noexcept;

        /*!
         * @return true if ActorId is a Core broadcast id
        */
        bool is_broadcast() const noexcept;

        /*!
         * @return true if ActorId is valid
        */
        bool is_valid() const noexcept;
    };

    class BroadcastId : public ActorId {
    public:
        BroadcastId() = delete;
        explicit BroadcastId(uint32_t const core_id) noexcept
            : ActorId(BroadcastSid, static_cast<CoreId>(core_id)) {}
    };

    using CoreIds = qb::unordered_set<CoreId>;
    using ActorIds = std::unordered_set<ActorId>;

}

namespace std {
    template<> struct hash<qb::ActorId> {
        std::size_t operator()(qb::ActorId const &val) const noexcept {
            return static_cast<uint32_t>(val);
        }
    };
}

qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::ActorId const &id);

#endif //QB_ACTORID_H
