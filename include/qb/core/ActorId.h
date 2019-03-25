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
# include <qb/utility/prefix.h>
# include <qb/io.h>

namespace qb {

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

        uint16_t _id;
        uint16_t _index;

    protected:
        ActorId(uint16_t const id, uint16_t const index) noexcept;
    public:
        static constexpr uint32_t NotFound = 0;
        static constexpr uint16_t BroadcastSid = std::numeric_limits<uint16_t>::max();

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

        operator const uint32_t &() const noexcept;

        /*!
         * @return Service index
         */
        uint16_t sid() const noexcept;
        /*!
         * @return VirtualCore index
         */
        uint16_t index() const noexcept;

        bool isBroadcast() const noexcept;
    };

    class BroadcastId : public ActorId {
    public:
        BroadcastId() = delete;
        explicit BroadcastId(uint32_t const core_id) noexcept
            : ActorId(BroadcastSid, static_cast<uint16_t>(core_id)) {}
    };

    using CoreIds = std::unordered_set<uint8_t>;
    using ActorIds = std::unordered_set<uint32_t>;

}

qb::io::stream &operator<<(qb::io::stream &os, qb::ActorId const &id);

#endif //QB_ACTORID_H
