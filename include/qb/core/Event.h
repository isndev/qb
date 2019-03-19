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

#ifndef QB_EVENT_H
# define QB_EVENT_H
# include <utility>
# include <bitset>
// include from qb
# include "ActorId.h"

namespace qb {

    template<typename T>
    struct type {
        constexpr static void id() {}
    };

    template<typename T>
    constexpr uint16_t type_id() { return static_cast<uint16_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    /*!
     * @class Event core/Event.h qb/event.h
     * @ingroup Core
     * @brief Event base class
     */
    class Event {
        friend class Main;
        friend class VirtualCore;
        friend class Actor;
        friend class ProxyPipe;
        friend struct ServiceEvent;

        uint16_t id;
        uint16_t bucket_size;
        std::bitset<32> state;
        // for users
        ActorId dest;
        ActorId source;

    public:
        Event() = default;

        inline ActorId getDestination() const { return dest; }
        inline ActorId getSource() const { return source; }
    };

    /*!
     * @class ServiceEvent core/Event.h qb/event.h
     * @ingroup Core
     * @brief More flexible Event
     * @details
     * Section in construction
     */
    struct ServiceEvent : public Event {
        ActorId forward;
        uint16_t service_event_id;

        inline void received() {
            std::swap(dest, forward);
            std::swap(id, service_event_id);
            live(true);
        }

        inline void live(bool flag) {
            state[0] = flag;
        }

        inline bool isLive() { return state[0]; }

        inline uint16_t bucketSize() const {
            return bucket_size;
        }
    };

    /*!
     * @class KillEvent core/Event.h qb/event.h
     * default registered event to kill Actor by event
     */
    struct KillEvent : public Event {};

    enum class ActorStatus : uint32_t {
        Alive,
        Dead
    };

    /*!
     * @private
     * @class PingEvent core/Event.h qb/event.h
     * default registered event to kill Actor by event
     */
    struct PingEvent : public Event {
        const uint32_t type;

        explicit PingEvent(uint32_t const actor_type)
            : type(actor_type)
        {}
    };

    struct RequireEvent : public Event {
        const uint32_t type;
        const ActorStatus status;

        explicit RequireEvent(uint32_t const actor_type, ActorStatus const actor_status)
                : type(actor_type), status(actor_status)
        {}
    };

} // namespace qb

#endif //QB_EVENT_H
