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
 * @ingroup Event
 */

#include <mutex>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>

#ifndef QB_EVENT_ROUTER_H
#define QB_EVENT_ROUTER_H

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
     * If the event type has an is_alive() method, it will be checked
     * before invoking the handler.
     *
     * @tparam _Handler Handler type
     * @tparam _Event Event type
     * @param handler The handler to invoke
     * @param event The event to pass to the handler
     */
    template <typename _Handler, typename _Event>
    inline void
    invoke(_Handler &handler, _Event &event) const {
        if constexpr (has_member_func_is_alive<_Event>::value) {
            if (handler.is_alive())
                handler.on(event);
        } else
            handler.on(event);
    }

    /**
     * @brief Dispose of an event if necessary
     * 
     * If the event type is not trivially destructible and has an is_alive() method,
     * it will be destructed when is_alive() returns false.
     *
     * @tparam _Event Event type
     * @param event The event to dispose
     */
    template <typename _Event>
    inline void
    dispose(_Event &event) const noexcept {
        if constexpr (!std::is_trivially_destructible_v<_Event>) {
            if constexpr (has_member_func_is_alive<_Event>::value) {
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
    using _EventId = typename _RawEvent::id_type;
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
    using _EventId = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    qb::unordered_map<_HandlerId, _Handler &> _subscribed_handlers;

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
        if constexpr (has_member_func_is_broadcast<_HandlerId>::value) {
            if (event.getDestination().is_broadcast()) {
                for (auto &it : _subscribed_handlers)
                    invoke(it.second, event);

                if constexpr (_CleanEvent)
                    dispose(event);

                return;
            }
        }

        const auto &it = _subscribed_handlers.find(event.dest);
        if (likely(it != _subscribed_handlers.cend()))
            invoke(it->second, event);

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
        _subscribed_handlers.insert({handler.id(), handler});
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
 * @brief Single-Event Multiple-Handler router (heterogeneous version)
 * 
 * Specialization that supports different handler types for the same event type.
 *
 * @tparam _RawEvent The event type
 */
template <typename _RawEvent>
class semh<_RawEvent, void> : public internal::EventPolicy {
    using _EventId = typename _RawEvent::id_type;
    using _HandlerId = typename _RawEvent::id_handler_type;

    /**
     * @brief Interface for handler resolution
     * 
     * Abstracts the process of resolving and invoking a handler
     * for heterogeneous handler types.
     */
    class IHandlerResolver {
    public:
        virtual ~IHandlerResolver() = default;

        /**
         * @brief Resolve and invoke a handler for the event
         *
         * @param event The event to route
         */
        virtual void resolve(_RawEvent &event) = 0;
    };

    /**
     * @brief Concrete handler resolver for a specific handler type
     *
     * @tparam _Handler The handler type
     */
    template <typename _Handler>
    class HandlerResolver
        : public IHandlerResolver
        , public sesh<_RawEvent, _Handler> {
    public:
        HandlerResolver() = delete;
        ~HandlerResolver() = default;

        /**
         * @brief Constructor with a handler
         *
         * @param handler The handler to resolve
         */
        explicit HandlerResolver(_Handler &handler) noexcept
            : sesh<_RawEvent, _Handler>(handler) {}

        /**
         * @brief Resolve and invoke the handler
         *
         * @param event The event to route
         */
        void
        resolve(_RawEvent &event) final {
            sesh<_RawEvent, _Handler>::template route<false>(event);
        }
    };

    qb::unordered_map<_HandlerId, IHandlerResolver &> _subscribed_handlers;

public:
    semh() = default;

    /**
     * @brief Destructor that cleans up all handler resolvers
     */
    ~semh() noexcept {
        for (const auto &it : _subscribed_handlers)
            delete &(it.second);
    }

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
        if constexpr (has_member_func_is_broadcast<_HandlerId>::value) {
            if (event.getDestination().is_broadcast()) {
                for (const auto &it : _subscribed_handlers)
                    it.second.resolve(event);

                if constexpr (_CleanEvent)
                    dispose(event);

                return;
            }
        }

        const auto &it = _subscribed_handlers.find(event.getDestination());
        if (likely(it != _subscribed_handlers.cend()))
            it->second.resolve(event);

        if constexpr (_CleanEvent)
            dispose(event);
    }

    /**
     * @brief Subscribe a handler to receive events
     * 
     * Creates a handler resolver for the specific handler type
     *
     * @tparam _Handler The handler type
     * @param handler The handler to subscribe
     */
    template <typename _Handler>
    void
    subscribe(_Handler &handler) noexcept {
        const auto &it = _subscribed_handlers.find(handler.id());
        if (it != _subscribed_handlers.cend()) {
            delete &it->second;
            _subscribed_handlers.erase(it);
        }
        _subscribed_handlers.insert(
            {handler.id(), *new HandlerResolver<_Handler>(handler)});
    }

    /**
     * @brief Unsubscribe a handler by ID
     *
     * @param id The ID of the handler to unsubscribe
     */
    void
    unsubscribe(_HandlerId const &id) noexcept {
        const auto &it = _subscribed_handlers.find(id);
        if (it != _subscribed_handlers.end()) {
            delete &it->second;
            _subscribed_handlers.erase(it);
        }
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
    using _EventId = typename _RawEvent::id_type;
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

    _Handler &_handler;
    qb::unordered_map<_EventId, IEventResolver &> _registered_events;

public:
    mesh() = delete;

    /**
     * @brief Constructs a MESH router with the given handler
     *
     * @param handler The handler that will receive events
     */
    explicit mesh(_Handler &handler) noexcept
        : _handler(handler) {}

    /**
     * @brief Destructor that cleans up all event resolvers
     */
    ~mesh() noexcept {
        unsubscribe();
    }

    /**
     * @brief Routes an event to the handler
     * 
     * Uses the event ID to find the appropriate resolver and process the event.
     * 
     * @param event The event to route
     */
    void
    route(_RawEvent &event) {
        // /!\ Why do not not protecting this ?
        // because a dynamic event pushed and not registred
        // has 100% risk of leaking memory
        // Todo : may be should add a define to prevent user from this
        // const auto &it = _registered_events.find(event.getID());
        // if (likely(it != _registered_events.cend()))
        //    it->second.resolve(event);
        _registered_events.at(event.getID()).resolve(_handler, event);
    }

    /**
     * @brief Subscribe to events of a specific type
     * 
     * Creates and registers an event resolver for the given event type.
     * 
     * @tparam _Event The event type to subscribe to
     */
    template <typename _Event>
    void
    subscribe() {
        const auto &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it == _registered_events.cend()) {
            _registered_events.insert(
                {_RawEvent::template type_to_id<_Event>(), *new EventResolver<_Event>});
        }
    }

    /**
     * @brief Unsubscribe from events of a specific type
     * 
     * Removes the event resolver for the given event type.
     * 
     * @tparam _Event The event type to unsubscribe from
     */
    template <typename _Event>
    void
    unsubscribe() {
        const auto &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it != _registered_events.cend()) {
            delete &it->second;
            _registered_events.erase(it);
        }
    }

    /**
     * @brief Unsubscribe from all event types
     * 
     * Removes all event resolvers.
     */
    void
    unsubscribe() {
        for (const auto &it : _registered_events)
            delete &it.second;
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
    using _EventId = typename _RawEvent::id_type;
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

    qb::unordered_map<_EventId, IEventResolver &> _registered_events;

public:
    memh() = default;

    /**
     * @brief Destructor that cleans up all event resolvers
     */
    ~memh() noexcept {
        for (const auto &it : _registered_events)
            delete &it.second;
    }

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
        if (likely(it != _registered_events.cend()))
            it->second.resolve(event);
        else {
            // std::lock_guard lk(_disposers_mtx);
            onError(event);
            //_disposers.at(event.getID())->dispose(&event);
        };
        // /!\ Look notice in of mesh router above
        // _registered_events.at(event.getID()).resolve(event);
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
        const auto &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it == _registered_events.cend()) {
            auto &resolver = *new EventResolver<_Event>;
            resolver.subscribe(handler);
            _registered_events.insert(
                {_RawEvent::template type_to_id<_Event>(), resolver});
        } else {
            dynamic_cast<EventResolver<_Event> &>(it->second).subscribe(handler);
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
        auto const &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it != _registered_events.cend())
            it->second.unsubscribe(handler.id());
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
        for (auto const &it : _registered_events) {
            it.second.unsubscribe(id);
        }
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
    using _EventId = typename _RawEvent::id_type;
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
                (void)event;
            }
        }
    };

    static inline qb::unordered_map<_EventId, std::unique_ptr<IDisposer>> _disposers;
    static inline std::mutex _disposers_mtx;

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

    qb::unordered_map<_EventId, IEventResolver &> _registered_events;

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
        /**
         * @brief Constructor that registers a disposer
         */
        SafeDispose() {
            std::lock_guard lk(_disposers_mtx);

            _disposers.try_emplace(_RawEvent::template type_to_id<T>(),
                                   new Disposer<T>());
        }

        ~SafeDispose() = default;
    };

    memh() = default;

    /**
     * @brief Destructor that cleans up all event resolvers
     */
    ~memh() noexcept {
        for (const auto &it : _registered_events)
            delete &it.second;
    }

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
        if (likely(it != _registered_events.cend()))
            it->second.resolve(event);
        else {
            onError(event);
            if constexpr (_CleanEvent) {
                std::lock_guard lk(_disposers_mtx);
                _disposers.at(event.getID())->dispose(&event);
            }
        };
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

        const auto &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it == _registered_events.cend()) {
            auto &resolver = *new EventResolver<_Event>;
            resolver.subscribe(handler);
            _registered_events.insert(
                {_RawEvent::template type_to_id<_Event>(), resolver});
        } else {
            dynamic_cast<EventResolver<_Event> *>(&(it->second))->subscribe(handler);
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
        auto const &it =
            _registered_events.find(_RawEvent::template type_to_id<_Event>());
        if (it != _registered_events.cend())
            it->second.unsubscribe(handler.id());
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
        for (auto const &it : _registered_events) {
            it.second.unsubscribe(id);
        }
    }
};

} // namespace qb::router

#endif // QB_EVENT_ROUTER_H
