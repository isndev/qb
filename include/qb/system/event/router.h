/**
 * @file qb/system/event/router.h
 * @brief Event routing system
 *
 * This file defines a flexible and type-safe event routing system that enables
 * message passing between components. It provides several router implementations
 * for different communication patterns:
 * - Single-Event Single-Handler (SESH) for direct point-to-point communication
 * - Single-Event Multiple-Handler (SEMH) for one-to-many distribution
 * - Multiple-Event Single-Handler (MESH) for many-to-one handling
 * - Multiple-Event Multiple-Handler (MEMH) for fully dynamic event routing
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
 * @ingroup Event
 */

#ifndef QB_EVENT_ROUTER_H
#define QB_EVENT_ROUTER_H

#include <memory>
#include <mutex>
#include <vector>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>

namespace qb::router {
//        struct EventExample {
//            using id_type = uint16_t;
//            using id_handler_type = uint32_t;
//
//            template<typename T>
//            constexpr id_type type_to_id();
//
//            id_type id;
//            id_handler_type dest;
//            id_handler_type source;
//        };

namespace internal {

/**
 * @brief Base policy for event handling
 *
 * Defines common event handling operations like invocation and disposal.
 */
class EventPolicy {
public:
    EventPolicy() = default;

    ~EventPolicy() = default;

protected:
    /**
     * @brief Invoke a handler with an event
     *
     * If the event type satisfies qb::has_is_alive, handler liveness
     * is checked before dispatch.
     *
     * @tparam _Handler Handler type
     * @tparam _Event Event type
     * @param handler The handler to invoke
     * @param event The event to pass to the handler
     */
    template <typename _Handler, typename _Event>
    inline void
    invoke(_Handler &handler, _Event &event) const {
        // C++20: use concept directly instead of trait with ::value
        if constexpr (qb::has_is_alive<_Event>) {
            if (handler.is_alive())
                handler.on(event);
        } else
            handler.on(event);
    }

    /**
     * @brief Dispose of an event if necessary
     *
     * Non-trivially destructible events are explicitly destructed.
     * If the event satisfies qb::has_is_alive, destruction only occurs
     * when is_alive() returns false.
     *
     * @tparam _Event Event type
     * @param event The event to dispose
     */
    template <typename _Event>
    inline void
    dispose(_Event &event) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<_Event>) {
            // C++20: use concept directly instead of trait with ::value
            if constexpr (qb::has_is_alive<_Event>) {
                if (!event.is_alive())
                    event.~_Event();
            } else
                event.~_Event();
        }
    }
};

} // namespace internal

/**
 * @brief Single-Event Single-Handler router
 *
 * Routes a specific event type to a single handler.
 *
 * @tparam _RawEvent The event type
 * @tparam _Handler The handler type
 */
template <typename _RawEvent, typename _Handler>
class sesh : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    _Handler &_handler;

public:
    sesh() = delete;

    /**
     * @brief Constructs a SESH router with the given handler
     *
     * @param handler The handler that will receive events
     */
    explicit sesh(_Handler &handler) noexcept
        : _handler(handler) {}

    /**
     * @brief Routes an event to the handler
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = true>
    void
    route(_RawEvent &event) {
        invoke(_handler, event);
        if constexpr (_CleanEvent)
            dispose(event);
    }
};

/**
 * @brief Single-Event Multiple-Handler router (generic version)
 *
 * Routes a specific event type to multiple handlers based on destination IDs.
 *
 * @tparam _RawEvent The event type
 * @tparam _Handler The handler type (void for heterogeneous handlers)
 */
template <typename _RawEvent, typename _Handler = void>
class semh : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    qb::unordered_map<_HandlerId, _Handler *> _subscribed_handlers;

public:
    semh() = default;

    ~semh() = default;

    /**
     * @brief Routes an event to the appropriate handler
     *
     * If the event has a broadcast destination, it's sent to all handlers.
     * Otherwise, it's sent to the handler that matches the destination ID.
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = true>
    void
    route(_RawEvent &event) noexcept {
        // C++20: use concept directly
        if constexpr (qb::has_is_broadcast<_HandlerId>) {
            if (event.getDestination().is_broadcast()) {
                // Snapshot before dispatch: a handler may (un)subscribe on this
                // map mid-broadcast (e.g. spawning an actor), which rehashes and
                // reallocates the release flat map (ska) and invalidates a live
                // iterator. See the heterogeneous route() below for the full
                // rationale; handlers are not destroyed until end-of-frame and
                // invoke() re-checks is_alive(), so the snapshot stays valid.
                static thread_local std::vector<_Handler *> bcast_snapshot;
                const std::size_t                           base = bcast_snapshot.size();
                for (auto &it : _subscribed_handlers)
                    bcast_snapshot.push_back(it.second);
                const std::size_t end = bcast_snapshot.size();
                for (std::size_t i = base; i < end; ++i)
                    invoke(*bcast_snapshot[i], event);
                bcast_snapshot.resize(base);

                if constexpr (_CleanEvent)
                    dispose(event);

                return;
            }
        }

        const auto &it = _subscribed_handlers.find(event.dest);
        if (likely(it != _subscribed_handlers.cend()))
            invoke(*it->second, event);

        if constexpr (_CleanEvent)
            dispose(event);
    }

    /**
     * @brief Subscribe a handler to receive events
     *
     * @param handler The handler to subscribe
     */
    void
    subscribe(_Handler &handler) noexcept {
        _subscribed_handlers.erase(handler.id());
        _subscribed_handlers.insert({handler.id(), &handler});
    }

    /**
     * @brief Unsubscribe a handler by ID
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) noexcept {
        _subscribed_handlers.erase(id);
    }
};

/**
 * @brief Single-Event Multiple-Handler router (heterogeneous version).
 *
 * Specialization that supports different handler types for the same event type.
 *
 * @tparam _RawEvent The event type
 *
 * @details
 * **Dispatch strategy (finding 2.7):** instead of storing a virtual
 * `IHandlerResolver` per subscribed handler — which costs one heap allocation
 * per subscription and one vtable lookup per dispatch — this implementation
 * stores a pair `{ void* handler, trampoline_fn }`. The trampoline is a
 * `static` function instantiated per handler type that performs the
 * `static_cast<_Handler*>` and calls `handler.on(event)` directly.
 *
 * This saves:
 * - 1 heap allocation per `subscribe<_Handler>()` call.
 * - 1 indirect virtual call per routed event.
 * - A vtable pointer (8 bytes) in every subscription entry.
 */
template <typename _RawEvent>
class semh<_RawEvent, void> : public internal::EventPolicy {
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    using Trampoline = void (*)(void *, _RawEvent &) noexcept;

    struct Entry {
        void      *handler  = nullptr;
        Trampoline dispatch = nullptr;
    };

    /**
     * @brief Typed trampoline: performs the `void*` → `_Handler*` recast and
     *        invokes the handler's `on(event)` method, honouring the
     *        `has_is_alive` protocol.
     */
    template <typename _Handler>
    static void
    dispatch_trampoline(void *opaque_handler, _RawEvent &event) noexcept {
        auto &handler = *static_cast<_Handler *>(opaque_handler);
        if constexpr (qb::has_is_alive<_RawEvent>) {
            if (handler.is_alive())
                handler.on(event);
        } else {
            handler.on(event);
        }
    }

    qb::unordered_map<_HandlerId, Entry> _subscribed_handlers;

public:
    semh()           = default;
    ~semh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handler
     *
     * If the event has a broadcast destination, it's sent to all handlers.
     * Otherwise, it's sent to the handler that matches the destination ID.
     *
     * @tparam _CleanEvent Whether to clean up the event after routing
     * @param event The event to route
     */
    template <bool _CleanEvent = false>
    void
    route(_RawEvent &event) const noexcept {
        if constexpr (qb::has_is_broadcast<_HandlerId>) {
            if (event.getDestination().is_broadcast()) {
                // A handler invoked here may (un)subscribe on THIS map — e.g.
                // spawning an actor registers KillEvent, inserting a new entry.
                // With the release flat map (ska) that insert rehashes and
                // REALLOCATES the entry array, invalidating a live range-for
                // iterator (heap-use-after-free — invisible to the debug
                // std::unordered_map used by the sanitizer presets, so it only
                // bites in release). Snapshot the targets first, then dispatch:
                // handlers are not destroyed until end-of-frame (removal is
                // deferred) and the trampoline re-checks is_alive(), so the
                // snapshotted pointers stay valid. A thread_local buffer with
                // base/restore keeps nested broadcasts allocation-free and
                // correct (each nested route pushes/pops its own [base,end)).
                static thread_local std::vector<Entry> bcast_snapshot;
                const std::size_t                      base = bcast_snapshot.size();
                for (const auto &it : _subscribed_handlers)
                    bcast_snapshot.push_back(it.second);
                const std::size_t end = bcast_snapshot.size();
                for (std::size_t i = base; i < end; ++i) {
                    // `subscribe()` always sets both fields atomically, so the
                    // function pointer is never null for a live entry (finding
                    // 2.17, C++23 `[[assume]]`); load into locals so the
                    // predicate is side-effect-free.
                    const auto  dispatch = bcast_snapshot[i].dispatch;
                    auto *const target   = bcast_snapshot[i].handler;
                    QB_ASSUME(dispatch != nullptr);
                    dispatch(target, event);
                }
                bcast_snapshot.resize(base);

                if constexpr (_CleanEvent)
                    dispose(event);

                return;
            }
        }

        const auto &it = _subscribed_handlers.find(event.getDestination());
        if (likely(it != _subscribed_handlers.cend())) {
            const auto  dispatch = it->second.dispatch;
            auto *const target   = it->second.handler;
            QB_ASSUME(dispatch != nullptr);
            dispatch(target, event);
        }

        if constexpr (_CleanEvent)
            dispose(event);
    }

    /**
     * @brief Subscribe a handler to receive events
     *
     * @tparam _Handler The handler type
     * @param handler The handler to subscribe
     */
    template <typename _Handler>
    void
    subscribe(_Handler &handler) noexcept {
        _subscribed_handlers[handler.id()] = Entry{static_cast<void *>(&handler), &dispatch_trampoline<_Handler>};
    }

    /**
     * @brief Unsubscribe a handler by ID
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) noexcept {
        _subscribed_handlers.erase(id);
    }
};

/**
 * @brief Multiple-Event Single-Handler router
 *
 * Routes multiple event types to a single handler based on event type IDs.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _Handler The handler type
 * @tparam _CleanEvent Whether to clean up events after routing
 */
template <typename _RawEvent, typename _Handler, bool _CleanEvent = true>
class mesh {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events
     * of different types.
     */
    class IEventResolver {
    public:
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and invoke a handler for the event
         *
         * @param handler The handler to invoke
         * @param event The event to route
         */
        virtual void resolve(_Handler &handler, _RawEvent &event) const = 0;
    };

    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public internal::EventPolicy {
    public:
        EventResolver() = default;

        /**
         * @brief Resolve and invoke a handler for the event
         *
         * @param handler The handler to invoke
         * @param event The event to route
         */
        void
        resolve(_Handler &handler, _RawEvent &event) const final {
            auto &revent = reinterpret_cast<_Event &>(event);
            invoke(handler, revent);
            if constexpr (_CleanEvent)
                dispose(revent);
        }
    };

    _Handler                                                    &_handler;
    qb::unordered_map<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

public:
    mesh() = delete;

    /**
     * @brief Constructs a MESH router with the given handler
     *
     * @param handler The handler that will receive events
     */
    explicit mesh(_Handler &handler) noexcept
        : _handler(handler) {}

    ~mesh() noexcept = default;

    /**
     * @brief Routes an event to the handler
     *
     * Unregistered dynamic events risk leaking memory, so this
     * intentionally throws via .at() on missing IDs.
     *
     * @param event The event to route
     */
    void
    route(_RawEvent &event) {
        _registered_events.at(event.getID())->resolve(_handler, event);
    }

    /**
     * @brief Subscribe to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     */
    template <typename _Event>
    void
    subscribe() {
        _registered_events.try_emplace(_RawEvent::template type_to_id<_Event>(), std::make_unique<EventResolver<_Event>>());
    }

    /**
     * @brief Unsubscribe from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     */
    template <typename _Event>
    void
    unsubscribe() {
        _registered_events.erase(_RawEvent::template type_to_id<_Event>());
    }

    /**
     * @brief Unsubscribe from all event types
     */
    void
    unsubscribe() {
        _registered_events.clear();
    }
};

/**
 * @brief Multiple-Event Multiple-Handler router (generic version)
 *
 * Routes multiple event types to multiple handlers based on event type and handler IDs.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _CleanEvent Whether to clean up events after routing
 * @tparam _Handler The handler type (void for heterogeneous handlers)
 */
template <typename _RawEvent, bool _CleanEvent = true, typename _Handler = void>
class memh {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events of different types.
     */
    class IEventResolver {
    public:
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        virtual void resolve(_RawEvent &event) = 0;

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        virtual void unsubscribe(_HandlerId const &id) = 0;
    };

    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public semh<_Event, _Handler> {
        using _HandlerId = typename _RawEvent::id_handler_type;

    public:
        /**
         * @brief Constructor
         */
        EventResolver() noexcept
            : semh<_Event, _Handler>() {}

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        void
        resolve(_RawEvent &event) final {
            auto &revent = reinterpret_cast<_Event &>(event);
            semh<_Event, _Handler>::template route<_CleanEvent>(revent);
        }

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        void
        unsubscribe(_HandlerId const &id) final {
            semh<_Event, _Handler>::unsubscribe(id);
        }
    };

    qb::unordered_map<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

public:
    memh()           = default;
    ~memh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handlers with error handling
     *
     * @tparam _Func Type of the error handling function
     * @param event The event to route
     * @param onError Function to call if the event type is not registered
     */
    template <typename _Func>
    void
    route(_RawEvent &event, _Func const &onError) const {
        const auto &it = _registered_events.find(event.getID());
        if (likely(it != _registered_events.cend())) {
            // Registered entries always own a valid unique_ptr; materialise the
            // raw pointer into a local so `QB_ASSUME` sees a side-effect-free
            // predicate (Clang's `-Wassume`) and can elide the null check.
            auto *const resolver = it->second.get();
            QB_ASSUME(resolver != nullptr);
            resolver->resolve(event);
        } else {
            onError(event);
        }
    }

    /**
     * @brief Subscribe a handler to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     * @param handler The handler to subscribe
     */
    template <typename _Event>
    void
    subscribe(_Handler &handler) {
        const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it == _registered_events.cend()) {
            auto resolver = std::make_unique<EventResolver<_Event>>();
            resolver->subscribe(handler);
            _registered_events.emplace(_RawEvent::template type_to_id<_Event>(), std::move(resolver));
        } else {
            dynamic_cast<EventResolver<_Event> *>(it->second.get())->subscribe(handler);
        }
    }

    /**
     * @brief Unsubscribe a handler from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     * @param handler The handler to unsubscribe
     */
    template <typename _Event>
    void
    unsubscribe(_Handler &handler) const {
        auto const &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it != _registered_events.cend())
            it->second->unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler from all event types
     *
     * @param handler The handler to unsubscribe
     */
    void
    unsubscribe(_Handler const &handler) const {
        unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler by ID from all event types
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) const {
        for (auto const &it : _registered_events)
            it.second->unsubscribe(id);
    }
};

/**
 * @brief Multiple-Event Multiple-Handler router (heterogeneous version)
 *
 * Specialization that supports different handler types for multiple event types.
 *
 * @tparam _RawEvent The raw event base type
 * @tparam _CleanEvent Whether to clean up events after routing
 */
template <typename _RawEvent, bool _CleanEvent>
class memh<_RawEvent, _CleanEvent, void> {
public:
    using _EventId   = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

private:
    /**
     * @brief Interface for event disposal
     *
     * Abstracts the process of cleaning up events of different types.
     */
    class IDisposer {
    public:
        virtual ~IDisposer() = default;

        /**
         * @brief Dispose of an event
         *
         * @param event The event to dispose
         */
        virtual void dispose(_RawEvent *event) = 0;
    };

    /**
     * @brief Concrete disposer for a specific event type
     *
     * @tparam T The specific event type
     */
    template <typename T>
    class Disposer : public IDisposer {
    public:
        /**
         * @brief Dispose of an event
         *
         * Calls the destructor for non-trivially destructible event types.
         *
         * @param event The event to dispose
         */
        void
        dispose(_RawEvent *event) final {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<T *>(event)->~T();
            } else {
                (void) event;
            }
        }
    };

    static inline qb::unordered_map<_EventId, std::unique_ptr<IDisposer>> _disposers;
    static inline std::mutex                                              _disposers_mtx;

    /**
     * @brief Per-router memo of resolved disposers, so the shared map is consulted once per type.
     * @details `_disposers` is `static` — one map for the whole process — and every read of it used
     *          to take `_disposers_mtx`. That lock sits on a path reached at full event rate: an
     *          event whose type has no resolver on this core falls into `route()`'s `else` branch,
     *          and `broadcast<E>()` produces exactly that on every core where no actor subscribed
     *          to `E` (`VirtualCore::__receive_events__`'s `onError` treats it as normal — it only
     *          logs for non-broadcast destinations). So a broadcasting system serialised all of its
     *          cores through one process-global mutex, which is the one thing a
     *          thread-per-`VirtualCore` engine must never do. Measured cost of a single lookup:
     *          9.0 ns at 1 thread, 24.3 at 2, 50.0 at 4, **146.7 ns at 8** — against a same-core
     *          dispatch budget of ~40-70 ns per event, and getting worse with every core added.
     *
     *          Caching makes the steady state a plain uncontended hash hit (~1-2 ns, flat in core
     *          count). It is sound because disposers are only ever ADDED — `try_emplace` into a
     *          `static` map of `unique_ptr` that lives for the whole program — so a memoised raw
     *          pointer can never dangle, and a type not yet registered is simply not cached and is
     *          retried on the next event.
     *
     *          Not synchronised, deliberately: `memh` is already a single-threaded-per-instance
     *          class (`_registered_events` is a plain map mutated by `subscribe`/`unsubscribe`);
     *          the `static` disposer map was its *only* shared state. Each `VirtualCore` owns its
     *          own router and touches it exclusively from its own thread.
     */
    mutable qb::unordered_map<_EventId, IDisposer *> _disposer_cache;

    /**
     * @brief Resolve the disposer for @p id, consulting the shared map at most once per type.
     * @return The disposer, or `nullptr` if none is registered yet.
     */
    [[nodiscard]] IDisposer *
    find_disposer(_EventId const &id) const {
        if (const auto it = _disposer_cache.find(id); likely(it != _disposer_cache.cend()))
            return it->second;

        IDisposer *resolved = nullptr;
        {
            std::lock_guard lk(_disposers_mtx);
            if (const auto dit = _disposers.find(id); dit != _disposers.cend())
                resolved = dit->second.get();
        }
        // Only memoise a hit: a miss means the type's disposer has not been registered yet, and
        // caching the null would make this router blind to it forever.
        if (resolved)
            _disposer_cache.emplace(id, resolved);
        return resolved;
    }

    /**
     * @brief Interface for event resolution
     *
     * Abstracts the process of resolving and handling events of different types.
     */
    class IEventResolver {
    public:
        virtual ~IEventResolver() = default;

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        virtual void resolve(_RawEvent &event) const = 0;

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        virtual void unsubscribe(_HandlerId const &id) = 0;
    };

    /**
     * @brief Concrete event resolver for a specific event type
     *
     * @tparam _Event The specific event type
     */
    template <typename _Event>
    class EventResolver
        : public IEventResolver
        , public semh<_Event> {
    public:
        /**
         * @brief Constructor
         */
        EventResolver() noexcept
            : semh<_Event>() {}

        /**
         * @brief Resolve and route an event to appropriate handlers
         *
         * @param event The event to route
         */
        void
        resolve(_RawEvent &event) const final {
            auto &revent = reinterpret_cast<_Event &>(event);
            semh<_Event>::template route<_CleanEvent>(revent);
        }

        /**
         * @brief Unsubscribe a handler by ID
         *
         * @param id The ID of the handler to unsubscribe
         */
        void
        unsubscribe(typename _RawEvent::id_handler_type const &id) final {
            semh<_Event>::unsubscribe(id);
        }
    };

    qb::unordered_map<_EventId, std::unique_ptr<IEventResolver>> _registered_events;

public:
    /**
     * @brief Helper to ensure safe disposal of events
     *
     * Registers a disposer for a specific event type.
     *
     * @tparam T The event type to register a disposer for
     */
    template <typename T>
    struct SafeDispose {
        SafeDispose() {
            std::lock_guard lk(_disposers_mtx);
            _disposers.try_emplace(_RawEvent::template type_to_id<T>(), std::make_unique<Disposer<T>>());
        }
        ~SafeDispose() = default;
    };

    memh()           = default;
    ~memh() noexcept = default;

    /**
     * @brief Routes an event to the appropriate handlers with error handling
     *
     * @tparam _Func Type of the error handling function
     * @param event The event to route
     * @param onError Function to call if the event type is not registered
     */
    template <typename _Func>
    void
    route(_RawEvent &event, _Func const &onError) const {
        const auto &it = _registered_events.find(event.getID());
        if (likely(it != _registered_events.cend())) {
            // Every entry inserted via `subscribe()` owns a valid resolver:
            // materialise the raw pointer into a local so `QB_ASSUME` sees a
            // side-effect-free predicate and can elide the null check around
            // the virtual call (finding 2.17).
            auto *const resolver = it->second.get();
            QB_ASSUME(resolver != nullptr);
            resolver->resolve(event);
        } else {
            onError(event);
            if constexpr (_CleanEvent) {
                // Free the payload of an event nobody subscribed to. The lookup is deliberately
                // tolerant (find, never `.at()`): `.at()` would throw std::out_of_range, and that
                // exception propagates out of VirtualCore::__receive_events__ / __workflow__ —
                // start_thread() catches it and flags ExceptionThrown, killing the whole
                // VirtualCore (every actor on it) for one misaddressed event.
                //
                // A missing disposer used to be the NORM here, not a corner case: disposers were
                // registered only by `subscribe<T>()`, so a type that was pushed but never
                // subscribed anywhere had none and this branch silently leaked its
                // `std::string` / `std::vector` members on every such event — unbounded, and
                // reachable by an ordinary refactor mistake. `router::ensure_disposer<Event, T>()`
                // now runs at every enqueue funnel (VirtualCore::push/send, Pipe::push/
                // allocated_push), so the disposer always exists by the time an event can reach
                // this branch. The find-and-skip stays as the no-throw safety net.
                //
                // This is the hot one: `broadcast<E>()` lands here on every core with no subscriber
                // for `E`. Goes through the per-router memo — see `_disposer_cache`.
                if (auto *const disposer = find_disposer(event.getID()))
                    disposer->dispose(&event);
            }
        }
    }

    /**
     * @brief Destroy the payload of an event that will NOT be routed.
     * @param event The event whose (possibly non-trivial) members must be destroyed.
     * @details Runs the same type-erased disposer `route()` applies after handling, for callers
     *          that own a raw event and decide to drop it without delivery — e.g.
     *          `VirtualCore::__pump_activations__` discarding an activation stash when an actor's
     *          async `onInit()` fails or it is killed during init, or dropping an event that
     *          overflowed the stash cap. Without it, a `push`'d event carrying a `std::string` /
     *          `std::vector` would leak its heap storage. The disposer is guaranteed to exist for
     *          any enqueued non-trivially-destructible type by `router::ensure_disposer<>()` at the
     *          enqueue funnels; the absent-disposer branch remains only as a no-throw safety net.
     */
    void
    dispose(_RawEvent &event) const {
        if (auto *const disposer = find_disposer(event.getID()))
            disposer->dispose(&event);
    }

    /**
     * @brief Subscribe a handler to events of a specific type
     *
     * @tparam _Event The event type to subscribe to
     * @tparam _Handler The handler type
     * @param handler The handler to subscribe
     */
    template <typename _Event, typename _Handler>
    void
    subscribe(_Handler &handler) {
        static const SafeDispose<_Event> o{};

        const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it == _registered_events.cend()) {
            auto resolver = std::make_unique<EventResolver<_Event>>();
            resolver->subscribe(handler);
            _registered_events.emplace(_RawEvent::template type_to_id<_Event>(), std::move(resolver));
        } else {
            dynamic_cast<EventResolver<_Event> *>(it->second.get())->subscribe(handler);
        }
    }

    /**
     * @brief Unsubscribe a handler from events of a specific type
     *
     * @tparam _Event The event type to unsubscribe from
     * @tparam _Handler The handler type
     * @param handler The handler to unsubscribe
     */
    template <typename _Event, typename _Handler>
    void
    unsubscribe(_Handler const &handler) const {
        auto const &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it != _registered_events.cend())
            it->second->unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler from all event types
     *
     * @tparam _Handler The handler type
     * @param handler The handler to unsubscribe
     */
    template <typename _Handler>
    void
    unsubscribe(_Handler const &handler) const {
        unsubscribe(handler.id());
    }

    /**
     * @brief Unsubscribe a handler by ID from all event types
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) const {
        for (auto const &it : _registered_events)
            it.second->unsubscribe(id);
    }
};

/**
 * @brief Guarantee a type-erased disposer exists for a NON-TRIVIALLY-DESTRUCTIBLE event type.
 *
 * The disposer registry that frees an event the framework must DROP (undeliverable residue at
 * shutdown, a stash overflow, or an event whose type no actor on the destination core subscribed
 * to) is populated only as a side effect of `memh::subscribe<E>()`. A type that is enqueued but
 * never subscribed anywhere therefore has NO disposer, so every drop path silently leaks its
 * `std::string` / `std::vector` members — reachable by ordinary means (a push to an actor that
 * does not handle that type). Call this at every ENQUEUE site: that is the one place which
 * statically knows each type entering the system.
 *
 * `if constexpr` emits NOTHING for the common trivially-destructible event; a non-trivial type
 * pays one function-local static guard (a well-predicted relaxed load) per enqueue.
 */
template <typename _RawEvent, typename _Event>
inline void
ensure_disposer() noexcept {
    if constexpr (!std::is_trivially_destructible_v<_Event>) {
        static const typename memh<_RawEvent>::template SafeDispose<_Event> registered{};
        (void) registered;
    }
}

} // namespace qb::router

#endif // QB_EVENT_ROUTER_H
