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

#ifndef QB_EVENT_H
#define QB_EVENT_H
#include <atomic>
#include <bitset>
#include <qb/system/allocator/pipe.h>
#include <utility>
// include from qb
#include "ActorId.h"
#include "ICallback.h"

namespace qb {

namespace detail {
/**
 * @brief Global, monotonically-increasing counter of distinct C++ types
 *        registered in the event/service system.
 * @details
 * Finding 2.1 — the legacy implementation derived the per-type `TypeId` from
 * the low 16 bits of the address of a static data member. With ASLR that is
 * effectively a random draw from 0..65535, and the birthday paradox makes
 * collisions ~50% likely around ~300 types in a single process. A silent
 * collision silently breaks event routing (two distinct types collapse to the
 * same slot in `router::memh`).
 *
 * The new strategy mirrors the `ServiceActor` registration fix (2.3): each
 * distinct `T` triggers a *single* post-increment of this atomic through the
 * magic-static barrier in `type_id_for<T>`. The resulting ID space is
 * sequential, dense and deterministic; collisions are impossible unless the
 * application registers strictly more than `std::numeric_limits<TypeId>::max()`
 * (65535) distinct event/service types — several orders of magnitude beyond
 * any realistic codebase.
 */
inline std::atomic<TypeId> _type_id_counter{0};

/**
 * @brief Per-type, magic-static unique identifier.
 * @details
 * Incrementing the global counter inside the initialiser of a function-local
 * static guarantees (per the C++ standard) that the bump happens exactly once
 * per `T`, even when multiple TUs race on first instantiation.
 */
template <typename T>
[[nodiscard]] inline TypeId
type_id_for() noexcept {
    static const TypeId id = _type_id_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return id;
}
} // namespace detail

/**
 * @struct type
 * @brief Tag template kept for source compatibility with legacy code.
 * @details
 * The original design encoded per-type identity in `&type<T>::id`. Identity
 * is now provided by `detail::type_id_for<T>()` (a dense counter-based ID),
 * but this empty tag is preserved so downstream code that may partial-
 * specialise `qb::type<T>` for traits continues to build unchanged.
 *
 * @tparam T The type to identify
 * @ingroup EventCore
 */
template <typename T>
struct type {
    inline static const char id = 0;
};

/**
 * @brief Return a unique 16-bit identifier for type `T`.
 * @details
 * Assigns a dense, collision-free `TypeId` the first time this function is
 * instantiated for `T` within the process. Identity is stable for the entire
 * program lifetime and safe across concurrent first calls (the magic-static
 * init barrier serialises the counter bump). Replaces the address-based
 * narrowing that was subject to ASLR collisions (finding 2.1).
 *
 * @tparam T The type to get an identifier for
 * @return A unique `TypeId` corresponding to the type `T`
 * @ingroup EventCore
 */
template <typename T>
[[nodiscard]] inline TypeId
type_id() noexcept {
    return detail::type_id_for<T>();
}

/*!
 * @class Event
 * @ingroup EventCore
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
     * @details Delegates to the collision-free, magic-static counter in
     *          `qb::detail::type_id_for<T>()` (finding 2.1). The previous
     *          ASLR-derived address narrowing is gone; event routing via
     *          `router::memh` is now immune to `EventId` collisions up to
     *          the 65535 distinct-types ceiling.
     */
    template <typename T>
    [[nodiscard]] static id_type
    type_to_id() noexcept {
        return qb::detail::type_id_for<T>();
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
        uint8_t prot[4] = {'q', 'b', '\0', 4 | ((QB_LOCKFREE_EVENT_BUCKET_BYTES / 16) << 3)};
    } state;
    uint16_t bucket_size;
    id_type  id;
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

/**
 * @typedef EventQOS2
 * @brief Event with highest quality of service (priority level 2)
 * @details
 * Events with QOS level 2 have the highest priority in the event system
 * and will be processed before events with lower QOS levels.
 * @ingroup EventCore
 */
using EventQOS2 = Event;

/**
 * @typedef EventQOS1
 * @brief Event with medium quality of service (priority level 1)
 * @details
 * Events with QOS level 1 have medium priority in the event system
 * and will be processed after QOS2 events but before QOS0 events.
 * @ingroup EventCore
 */
using EventQOS1 = Event;

/*!
 * @struct EventQOS0
 * @ingroup EventCore
 * @brief Event with lowest quality of service level
 */
struct EventQOS0 : public Event {
    EventQOS0() {
        state.qos = 0;
    }
};

/*!
 * @class ServiceEvent
 * @ingroup EventCore
 * @brief Event type for service-to-service communication
 * @details
 * ServiceEvent extends the base Event class with additional functionality
 * for service-to-service communication, including event forwarding and
 * service-specific event identification.
 */
struct ServiceEvent : public Event {
    id_handler_type forward;
    id_type         service_event_id;

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
 * @struct KillEvent
 * @ingroup EventCore
 * @brief Event used to terminate an actor
 */
struct KillEvent : public Event {};

/*!
 * @struct UnregisterCallbackEvent
 * @ingroup EventCore
 * @brief Event used to unregister an actor's callback
 */
struct UnregisterCallbackEvent : public Event {};

/*!
 * @struct SignalEvent
 * @ingroup EventCore
 * @brief Event used to handle system signals
 */
struct SignalEvent : public Event {
    int signum;
};

/**
 * @enum ActorStatus
 * @brief Represents the current status of an actor in the system
 * @details
 * This enumeration is used to indicate whether an actor is currently active (Alive)
 * or has been terminated/removed from the system (Dead). It is typically used in
 * status queries and responses between actors and the core system.
 *
 * @ingroup EventCore
 */
enum class ActorStatus : uint32_t {
    /** @brief The actor is active and operational */
    Alive,
    /** @brief The actor has been terminated or removed from the system */
    Dead
};

/*!
 * @struct PingEvent
 * @ingroup EventCore
 * @brief Event used for actor health checks
 */
struct PingEvent : public Event {
    const uint32_t type;

    explicit PingEvent(uint32_t const actor_type) noexcept
        : type(actor_type) {}
};

/*!
 * @struct RequireEvent
 * @ingroup EventCore
 * @brief Event used to query actor status
 */
struct RequireEvent : public Event {
    const uint32_t    type;
    const ActorStatus status;

    explicit RequireEvent(uint32_t const actor_type, ActorStatus const actor_status) noexcept
        : type(actor_type)
        , status(actor_status) {}
};

/*!
 * @struct WithData
 * @ingroup EventCore
 * @brief Event template that includes data payload
 * @tparam _Args Types of arguments for the data tuple.
 */
template <typename... _Args>
struct WithData : public Event {
    std::tuple<_Args...> data;

    explicit WithData(_Args &&...args)
        : data(std::forward<_Args>(args)...) {}
};

/*!
 * @struct WithoutData
 * @ingroup EventCore
 * @brief Event template without data payload
 * @tparam _Args Placeholder for template consistency, not used for data.
 */
template <typename... _Args>
class WithoutData : public Event {};

/*!
 * @struct AskData
 * @ingroup EventCore
 * @brief Event template for requesting data, typically without carrying data itself.
 * @tparam _Args Placeholder for template consistency, not used for data.
 */
template <typename... _Args>
struct AskData : public WithoutData<_Args...> {};

/*!
 * @struct FillEvent
 * @ingroup EventCore
 * @brief Event template for events that carry and "fill" data.
 * @tparam _Args Types of arguments for the data tuple.
 */
template <typename... _Args>
struct FillEvent : public WithData<_Args...> {
    using base_t = WithData<_Args...>;
    FillEvent()  = default;
    explicit FillEvent(_Args &&...args)
        : base_t(std::forward<_Args>(args)...) {}
};

/**
 * @typedef VirtualPipe
 * @brief Pipe for event transmission in the actor system
 * @details
 * A specialized pipe based on the allocator::pipe template that
 * is configured to handle EventBucket objects, which contain events
 * for transmission between actors and cores.
 * @ingroup EventCore
 */
using VirtualPipe = allocator::pipe<EventBucket>;

/**
 * @typedef event
 * @brief Alias for the base Event class
 * @details
 * Provided for naming consistency with other lowercase aliases
 * in the framework.
 * @ingroup EventCore
 */
using event = Event;

/**
 * @typedef service_event
 * @brief Alias for the ServiceEvent class
 * @details
 * Provided for naming consistency with other lowercase aliases
 * in the framework.
 * @ingroup EventCore
 */
using service_event = ServiceEvent;

} // namespace qb

#endif // QB_EVENT_H
