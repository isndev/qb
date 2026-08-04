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
// Explicit, though qb/system/allocator/pipe.h already reaches it: qb::Event is the type whose
// layout the cache-line axis moves, and every user event derives from it, so the header that
// declares it states its own link-time ABI contract rather than inheriting it by accident.
#include <qb/utility/abi.h>
#include <typeinfo>
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
 * @brief Record `name` as the human-readable name of the type that was assigned `id`.
 * @details
 * Back half of the side registry that replaced the Debug-only `const char *` event id.
 * Defined out of line in `libqb-core` (`Event.cpp`) over a **dense, direct-indexed table**:
 * ids are handed out by `_type_id_counter` as a dense sequence, and their domain is exactly
 * the `TypeId` domain, so a table with one slot per `TypeId` value resolves every id that
 * can ever exist in O(1) with no bounds check, no allocation, no mutex and no growth step.
 *
 * Out of line, not in this header, for two reasons. (1) A header-defined table is a *weak*
 * definition, and Mach-O refuses to place weak data in zero-fill: measured, an
 * `inline constinit std::array<std::atomic<char const *>, 65536>` costs **+512 KiB of file
 * size in every linked binary** (`__DATA,__data`), whereas the strong definition inside
 * `libqb-core` is `__bss` and costs 0 file bytes and one resident page. (2) It keeps the
 * table an implementation detail rather than an exported data symbol, which is what a
 * `QB_BUILD_SHARED_LIBS=ON` build on Windows would otherwise need `__declspec(dllimport)`
 * for — qb-core annotates no symbol today.
 *
 * @param id   The id just assigned to the type.
 * @param name `typeid(T).name()` — a link-time constant address, never a heap string.
 * @return `id`, unchanged, so the caller stays a one-expression initialiser.
 * @note Called exactly once per type per process, from inside the magic-static initialiser
 *       in `type_id_for<T>()`, i.e. from the outlined cold path. Nothing on a routing path
 *       calls it.
 */
TypeId register_type_name(TypeId id, char const *name) noexcept;

/**
 * @brief Reverse lookup: the human-readable name recorded for an assigned `TypeId`.
 * @details One direct-indexed atomic load. **Diagnostics only** — no routing path calls it,
 *          it is reached only from `LOG_*` sites that are already behind
 *          `nanolog::is_logged(level)`. Measured: it contributes 9 instructions
 *          (`adrp/add/add/ldapr/cmp/adrp/add/csel/str`) to the unroutable-event branch of
 *          `VirtualCore::__receive_events__`, where it inlines because `Event.cpp` is part
 *          of the same unity translation unit; the intrusive-list shape it replaced
 *          contributed a pointer-chasing walk instead.
 * @param id A value previously returned by `type_id_for<T>()` / `Event::getID()`.
 * @return The `typeid(T).name()` of the type owning `id`, or `"<unregistered>"`.
 */
[[nodiscard]] char const *type_name_for(TypeId id) noexcept;

/**
 * @brief Per-type, magic-static unique identifier.
 * @details
 * Incrementing the global counter inside the initialiser of a function-local
 * static guarantees (per the C++ standard) that the bump happens exactly once
 * per `T`, even when multiple TUs race on first instantiation.
 *
 * The same one-shot initialiser now also records `typeid(T).name()` in the side registry.
 * That is what keeps a human-readable event name available after `Event::id_type` stopped
 * *being* that name — see `qb::event_type_name()`. The static is still a bare `TypeId`, so
 * `id` is at offset 0 of an object whose size is 2: the guard test and the 16-bit load this
 * function compiles to are unchanged, and `typeid` materialises only inside the outlined
 * cold initialiser, reached once per type per process.
 *
 * @warning `T` must be a **complete** type. Before 3.0 this function never touched `typeid` in
 *          *either* build mode, so an incomplete `T` compiled everywhere it was reached through
 *          `qb::type_id<T>()` — `ServiceActor<Tag>`, `getServiceId<Tag>()`, `require<T>()`. It
 *          now fails in every build mode with `'typeid' of incomplete type`. (Only
 *          `Event::type_to_id<T>()` already rejected an incomplete `T`, and only in Debug, where
 *          it called `typeid(T).name()` directly.) Verified against 2.6 headers, both modes.
 */
template <typename T>
[[nodiscard]] inline TypeId
type_id_for() noexcept {
    static const TypeId id =
        register_type_name(static_cast<TypeId>(_type_id_counter.fetch_add(1, std::memory_order_relaxed) + 1), typeid(T).name());
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
 * @warning `T` must be a **complete** type in every build mode since 3.0 — the id assignment also
 *          records `typeid(T).name()`. `qb::ServiceActor<struct MyTag>` therefore no longer
 *          compiles: that spelling only *declares* the tag. Write `struct MyTag {};` first.
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

    /*!
     * @brief Routing key of an event — the same 16-bit type id in **every** build mode.
     * @details Until 3.0 this was `EventId` under `NDEBUG` and `const char *` otherwise, which
     *          put `id` at offset 6 (2 bytes) in Release and offset 8 (8 bytes, 8-aligned) in
     *          Debug, and therefore `dest`/`source` at 8/12 vs 16/20. Cross-core events are
     *          memcpy-relocated (`VirtualCore.cpp`, `reinterpret_cast<Event *>(buckets.data())`)
     *          and `libqb-core` is an installable package, so a consumer compiled with the other
     *          `NDEBUG` read `dest` at the wrong offset and routed to a garbage `ActorId`,
     *          silently. One representation, one layout.
     *
     *          The human-readable name did not go away — it moved to the side registry that
     *          `qb::detail::type_id_for<T>` fills when it assigns the id: see `type_to_name()`
     *          and `qb::event_type_name()`.
     */
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
     * @warning `T` must be a complete type; see `qb::detail::type_id_for<T>()`.
     */
    template <typename T>
    [[nodiscard]] static id_type
    type_to_id() noexcept {
        return qb::detail::type_id_for<T>();
    }
    /*!
     * @brief Get the human-readable name of an event type.
     * @tparam T Type to get the name for
     * @return `typeid(T).name()` — a link-time constant string, valid for the whole program.
     * @details What the Debug-only `const char *` id used to give by *being* the id. It is now a
     *          separate lookup, which is exactly why the on-the-wire layout no longer depends on
     *          `NDEBUG`. Available in every build mode; diagnostics only — nothing on the dispatch
     *          path reads it. The result is Itanium-mangled (`N2qb9KillEventE`), exactly as the
     *          Debug id printed before.
     */
    template <typename T>
    [[nodiscard]] static char const *
    type_to_name() noexcept {
        return typeid(T).name();
    }

private:
    union Header {
        /**
         * @brief Bit-field view of the 4-byte header word.
         * @details The bit-fields **must** live in a named struct member, never as bare union
         *          members: in a union every member is allocated at offset 0, and each bit-field
         *          declarator is its own member — so `alive` (bit 0), `qos` (bits 0-1) and
         *          `factor` (bits 0-4) would all alias one another **and** `prot[0]`. Writing
         *          `alive` would rewrite `qos`, `EventQOS0`'s `qos = 0` would clear `alive`, and
         *          the `'q'` of the magic would be mutated on every reply()/forward().
         *
         *          Inside a struct the `: 16, : 8` padding declarators do their job and place
         *          `alive` at bit 24 — i.e. `prot[3]`, the one byte the default member
         *          initializer below actually encodes (`4` → `qos = 2`, `<< 3` → `factor =
         *          bucket_bytes / 16`, `alive = 0`). `prot[0..2]` ("qb\0") stays an untouched
         *          magic for the whole life of the event.
         */
        struct {
            uint32_t : 16, : 8, alive : 1, qos : 2, factor : 5;
        } bits;
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
        return state.bits.alive;
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
        return state.bits.qos;
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

/*!
 * @brief Human-readable name of the event type behind a runtime `Event::id_type`.
 * @param id A value obtained from `Event::getID()`.
 * @return The registered `typeid(T).name()`, or `"<unregistered>"` if `id` names no type this
 *         process has ever assigned (a corrupted event, or the reserved id `0`).
 * @details This is the replacement for "in Debug the id *is* the name". One direct-indexed
 *          lookup, meant for log lines and assertions only — the router never calls it.
 * @ingroup EventCore
 */
[[nodiscard]] inline char const *
event_type_name(Event::id_type const id) noexcept {
    return detail::type_name_for(id);
}

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
        state.bits.qos = 0;
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
        state.bits.alive = flag;
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
 * @struct CorrelatedEvent
 * @ingroup EventCore
 * @brief Base for any event that carries a `correlation_id` resolving a pending coroutine
 *        continuation (an `ask` reply, a stream chunk, a discovery reply, …).
 * @details The per-core continuation registry routes a reply to the awaiting coroutine by its
 *          `correlation_id`. Because every correlated reply derives from `CorrelatedEvent` as its
 *          first base, the activation gate can read `correlation_id` at a single fixed offset (no
 *          RTTI) and deliver the reply to an actor that is still *Activating* — so the **whole**
 *          coroutine pattern library (`ask`/`ask_stream`/`ping`/`require`/…) works inside `onInit()`.
 *          `0` means "not correlated" (legacy / fire-and-forget).
 */
struct CorrelatedEvent : public Event {
    std::uint64_t correlation_id{0};
};

/*!
 * @struct SignalEvent
 * @ingroup EventCore
 * @brief Event used to handle system signals
 */
struct SignalEvent : public Event {
    int signum;
};

/*!
 * @struct PingEvent
 * @ingroup EventCore
 * @brief Event used for actor health checks / discovery.
 * @details `type` is the discovery target type id; the special value `0` is a **wildcard**
 *          (any live actor replies — used by the targeted liveness `qb::ping`). The inherited
 *          `correlation_id` (0 = none) is echoed back in the `RequireEvent` reply so the coroutine
 *          `qb::ping` / `qb::require` helpers can match it; the legacy broadcast `require<...>()`
 *          leaves it 0. Deriving `CorrelatedEvent` keeps the id at the uniform base offset.
 */
struct PingEvent : public CorrelatedEvent {
    const uint32_t type;

    explicit PingEvent(uint32_t const actor_type) noexcept
        : type(actor_type) {}
    PingEvent(uint32_t const actor_type, uint64_t const corr) noexcept
        : type(actor_type) {
        correlation_id = corr; // inherited from CorrelatedEvent
    }
};

/*!
 * @struct RequireEvent
 * @ingroup EventCore
 * @brief Discovery reply to a `PingEvent` — a live actor of `type` announces itself to the asker.
 * @details Presence IS the status (a dead actor never replies), so there is no `status` field; the
 *          `correlation_id` (from `CorrelatedEvent`) matches it to a pending `co_await qb::require` /
 *          `qb::ping`.
 */
struct RequireEvent : public CorrelatedEvent {
    const uint32_t type;

    explicit RequireEvent(uint32_t const actor_type) noexcept
        : type(actor_type) {}
    RequireEvent(uint32_t const actor_type, uint64_t const corr) noexcept
        : type(actor_type) {
        correlation_id = corr; // inherited from CorrelatedEvent
    }
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
