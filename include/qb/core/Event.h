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
# include "ICallback.h"

namespace qb {

    template<typename T>
    struct type {
        constexpr static void id() {}
    };

    template<typename T>
    constexpr TypeId type_id() { return static_cast<TypeId>(reinterpret_cast<std::size_t>(&type<T>::id)); }

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
        friend struct EventQOS0;
        friend struct ServiceEvent;
    public:
        using id_handler_type = ActorId;

#ifdef NDEBUG
        using id_type = EventId;
        template<typename T>
        constexpr static id_type type_to_id() { return static_cast<id_type>(reinterpret_cast<std::size_t>(&qb::type<T>::id)); }
#else
        using id_type = const char *;
        template<typename T>
        constexpr static id_type type_to_id() { return typeid(T).name(); }
#endif

    private:
        union {
            struct {
                uint32_t
                        :16,
                        :8,
                        alive:1,
                        qos:2,
                        factor:5;
            };
            uint8_t prot[4] = {'q','b','\0', 4 | ((QB_LOCKFREE_EVENT_BUCKET_BYTES / 16) << 3) };
        } state;
        uint16_t bucket_size;
        id_type id;
        // for users
        id_handler_type dest;
        id_handler_type source;

    public:

        Event() noexcept = default;

        inline bool is_alive() const noexcept { return state.alive; }
        inline id_type getID() const noexcept { return id; }
        inline uint8_t getQOS() const noexcept { return state.qos; }
        inline id_handler_type getDestination() const noexcept { return dest; }
        inline id_handler_type getSource() const noexcept { return source; }
    };

    using EventQOS2 = Event;
    using EventQOS1 = Event;

    struct EventQOS0 : public Event {
        EventQOS0() { state.qos = 0; }
    };

    /*!
     * @class ServiceEvent core/Event.h qb/event.h
     * @ingroup Core
     * @brief More flexible Event
     * @details
     * Section in construction
     */
    struct ServiceEvent : public Event {
        id_handler_type forward;
        id_type service_event_id;

        inline void received() noexcept {
            std::swap(dest, forward);
            std::swap(id, service_event_id);
            live(true);
        }

        inline void live(bool flag) noexcept {
            state.alive = flag;
        }

        inline bool isLive() const noexcept { return state.alive; }

        inline uint16_t bucketSize() const noexcept {
            return bucket_size;
        }
    };

    /*!
     * @class KillEvent core/Event.h qb/event.h
     * default registered event to kill Actor by event
     */
    struct KillEvent : public Event {};
    struct UnregisterCallbackEvent : public Event {};

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

        explicit PingEvent(uint32_t const actor_type) noexcept
            : type(actor_type)
        {}
    };

    struct RequireEvent : public Event {
        const uint32_t type;
        const ActorStatus status;

        explicit RequireEvent(uint32_t const actor_type, ActorStatus const actor_status) noexcept
                : type(actor_type), status(actor_status)
        {}
    };

    template <typename ..._Args>
    struct WithData : public Event {
        std::tuple<_Args...> data;

        WithData(_Args &&...args)
            : data(std::forward<_Args>(args)...)
        {}
    };

    template <typename ..._Args>
    class WithoutData : public Event {
    };

    /*!
     * @private
     * @tparam Data
     */
    template <typename ..._Args>
    struct AskData : public WithoutData<_Args...> {
    };

    /*!
     * @private
     * @tparam Data
     */
    template <typename ..._Args>
    struct FillEvent : public WithData<_Args...>
    {
        using base_t = WithData<_Args...>;
        FillEvent() = default;
        explicit FillEvent(_Args &&...args)
            : base_t(std::forward<_Args>(args)...)
        {}
    };

} // namespace qb

#endif //QB_EVENT_H
