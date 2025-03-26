/**
 * @file qb/core/Event.h
 * @brief Event system for the QB Actor Framework
 * 
 * This file defines the event system used by the QB Actor Framework for
 * communication between actors. It includes the base Event class and several
 * specialized event types for different purposes, including quality of service
 * levels, service events, and system events like kill and signal events.
 * 
 * Events are the primary means of communication between actors, ensuring
 * isolation and thread safety in the actor system.
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

#ifndef QB_EVENT_H
#define QB_EVENT_H
#include <bitset>
#include <qb/system/allocator/pipe.h>
#include <utility>
// include from qb
#include "ActorId.h"
#include "ICallback.h"

namespace qb {

template <typename T>
struct type {
    constexpr static void
    id() {}
};

template <typename T>
constexpr TypeId
type_id() {
    return static_cast<TypeId>(reinterpret_cast<std::size_t>(&type<T>::id));
}

/*!
 * @class Event core/Event.h qb/event.h
 * @ingroup Core
 * @brief Base class for all events in the actor system
 * @details
 * Event provides the base functionality for event identification, routing,
 * and quality of service. It is the foundation for all event types in the
 * actor system.
 */
class QB_LOCKFREE_CACHELINE_ALIGNMENT Event {
    friend class SharedCoreCommunication;
    friend class VirtualCore;
    friend class Actor;
    friend class Pipe;
    friend struct EventQOS0;
    friend struct ServiceEvent;

public:
    using id_handler_type = ActorId;

#ifdef NDEBUG
    using id_type = EventId;
    /*!
     * @brief Get the type identifier at compile time
     * @tparam T Type to get the ID for
     * @return Type identifier for the specified type
     */
    template <typename T>
    [[nodiscard]] constexpr static id_type
    type_to_id() {
        return static_cast<id_type>(reinterpret_cast<std::size_t>(&qb::type<T>::id));
    }
#else
    using id_type = const char *;
    /*!
     * @brief Get the type identifier at runtime
     * @tparam T Type to get the ID for
     * @return Type identifier as string for the specified type
     */
    template <typename T>
    [[nodiscard]] constexpr static id_type
    type_to_id() {
        return typeid(T).name();
    }
#endif

private:
    union Header {
        uint32_t : 16, : 8, alive : 1, qos : 2, factor : 5;
        uint8_t prot[4] = {'q', 'b', '\0',
                           4 | ((QB_LOCKFREE_EVENT_BUCKET_BYTES / 16) << 3)};
    } state;
    uint16_t bucket_size;
    id_type id;
    // for users
    id_handler_type dest;
    id_handler_type source;

//    Event &operator=(Event const &) = default;

public:
    Event() = default;

    /*!
     * @brief Check if the event is still alive and can be processed
     * @return true if the event is alive and ready for processing, false otherwise
     */
    [[nodiscard]] inline bool
    is_alive() const noexcept {
        return state.alive;
    }
    /*!
     * @brief Get the event's type ID for event routing and handling
     * @return Type identifier of this event
     */
    [[nodiscard]] inline id_type
    getID() const noexcept {
        return id;
    }
    /*!
     * @brief Get the event's quality of service level
     * @return QoS level (0-2) where higher values indicate higher priority
     */
    [[nodiscard]] inline uint8_t
    getQOS() const noexcept {
        return state.qos;
    }
    /*!
     * @brief Get the destination actor ID
     * @return ID of the destination actor that should receive this event
     */
    [[nodiscard]] inline id_handler_type
    getDestination() const noexcept {
        return dest;
    }
    /*!
     * @brief Get the source actor ID
     * @return ID of the source actor that sent this event
     */
    [[nodiscard]] inline id_handler_type
    getSource() const noexcept {
        return source;
    }
    /*!
     * @brief Get the size of the event in bytes
     * @return Total size of the event in memory
     */
    [[nodiscard]] inline std::size_t
    getSize() const noexcept {
        return static_cast<std::size_t>(bucket_size) * QB_LOCKFREE_EVENT_BUCKET_BYTES;
    }
};

using EventQOS2 = Event;
using EventQOS1 = Event;

/*!
 * @struct EventQOS0 core/Event.h qb/event.h
 * @brief Event with lowest quality of service level
 */
struct EventQOS0 : public Event {
    EventQOS0() {
        state.qos = 0;
    }
};

/*!
 * @class ServiceEvent core/Event.h qb/event.h
 * @ingroup Core
 * @brief Event type for service-to-service communication
 * @details
 * ServiceEvent extends the base Event class with additional functionality
 * for service-to-service communication, including event forwarding and
 * service-specific event identification.
 */
struct ServiceEvent : public Event {
    id_handler_type forward;
    id_type service_event_id;

    /*!
     * @brief Mark the event as received and swap source/destination
     */
    inline void
    received() noexcept {
        std::swap(dest, forward);
        std::swap(id, service_event_id);
        live(true);
    }

    /*!
     * @brief Set the event's alive status
     * @param flag New alive status
     */
    inline void
    live(bool flag) noexcept {
        state.alive = flag;
    }
};

/*!
 * @struct KillEvent core/Event.h qb/event.h
 * @brief Event used to terminate an actor
 */
struct KillEvent : public Event {};

/*!
 * @struct UnregisterCallbackEvent core/Event.h qb/event.h
 * @brief Event used to unregister an actor's callback
 */
struct UnregisterCallbackEvent : public Event {};

/*!
 * @struct SignalEvent core/Event.h qb/event.h
 * @brief Event used to handle system signals
 */
struct SignalEvent : public Event {
    int signum;
};

enum class ActorStatus : uint32_t { Alive, Dead };

/*!
 * @struct PingEvent core/Event.h qb/event.h
 * @brief Event used for actor health checks
 */
struct PingEvent : public Event {
    const uint32_t type;

    explicit PingEvent(uint32_t const actor_type) noexcept
        : type(actor_type) {}
};

/*!
 * @struct RequireEvent core/Event.h qb/event.h
 * @brief Event used to query actor status
 */
struct RequireEvent : public Event {
    const uint32_t type;
    const ActorStatus status;

    explicit RequireEvent(uint32_t const actor_type,
                          ActorStatus const actor_status) noexcept
        : type(actor_type)
        , status(actor_status) {}
};

/*!
 * @struct WithData core/Event.h qb/event.h
 * @brief Event template that includes data payload
 */
template <typename... _Args>
struct WithData : public Event {
    std::tuple<_Args...> data;

    explicit WithData(_Args &&... args)
        : data(std::forward<_Args>(args)...) {}
};

/*!
 * @struct WithoutData core/Event.h qb/event.h
 * @brief Event template without data payload
 */
template <typename... _Args>
class WithoutData : public Event {};

/*!
 * @struct AskData core/Event.h qb/event.h
 * @brief Event template for requesting data
 */
template <typename... _Args>
struct AskData : public WithoutData<_Args...> {};

/*!
 * @struct FillEvent core/Event.h qb/event.h
 * @brief Event template for filling data
 */
template <typename... _Args>
struct FillEvent : public WithData<_Args...> {
    using base_t = WithData<_Args...>;
    FillEvent() = default;
    explicit FillEvent(_Args &&... args)
        : base_t(std::forward<_Args>(args)...) {}
};

using VirtualPipe = allocator::pipe<EventBucket>;
using event = Event;
using service_event = ServiceEvent;

} // namespace qb

#endif // QB_EVENT_H
