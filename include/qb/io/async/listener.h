/**
 * @file qb/io/async/listener.h
 * @brief Core event loop manager for the asynchronous IO framework
 * 
 * This file defines the listener class which serves as the central event loop manager
 * for the asynchronous IO framework. It provides thread-local access to the event loop,
 * registration and management of event handlers, and methods to run the event loop.
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
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_LISTENER_H_
#define QB_IO_ASYNC_LISTENER_H_

#include "event/base.h"
#include <algorithm>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <vector>

namespace qb::io::async {

/**
 * @class listener
 * @brief Central event loop manager for asynchronous IO operations
 * 
 * The listener class is the core of the asynchronous event system. It manages
 * an event loop that handles all asynchronous events (IO, timer, signal, etc.)
 * and dispatches them to registered handlers.
 * 
 * Each thread has its own listener instance accessible via the thread_local
 * static member 'current'.
 */
class listener {
public:
    /**
     * @brief Thread-local instance of the listener
     * 
     * Each thread has its own listener accessible through this static member.
     * This provides a way to access the current thread's event loop without
     * passing a reference explicitly.
     */
    thread_local static listener current;

    /**
     * @class RegisteredKernelEvent
     * @brief Template wrapper for event handlers
     * 
     * This class wraps an event and its associated actor (handler) and provides
     * a way to invoke the actor's handler method when the event is triggered.
     * 
     * @tparam _Event The event type
     * @tparam _Actor The actor (handler) type
     */
    template <typename _Event, typename _Actor>
    class RegisteredKernelEvent final : public IRegisteredKernelEvent {
        friend class listener;

        _Actor &_actor;      /**< Reference to the actor that handles the event */
        _Event _event;       /**< The event object */

        /**
         * @brief Destructor
         */
        ~RegisteredKernelEvent() final = default;

        /**
         * @brief Constructor
         * @param loop Reference to the event loop
         * @param actor Reference to the actor that will handle the event
         */
        explicit RegisteredKernelEvent(ev::loop_ref loop, _Actor &actor) noexcept
            : _actor(actor)
            , _event(loop) {}

        /**
         * @brief Invoke the actor's handler for this event
         * 
         * This method is called when the event is triggered. It checks if the
         * actor is still alive (if it provides an is_alive method) and calls
         * the actor's on() method with the event.
         */
        void
        invoke() final {
            if constexpr (has_member_func_is_alive<_Actor>::value) {
                if (likely(_actor.is_alive()))
                    _actor.on(_event);
            } else
                _actor.on(_event);
        }
    };

private:
    ev::dynamic_loop _loop;  /**< The libev event loop */
    qb::unordered_set<IRegisteredKernelEvent *> _registeredEvents; /**< Set of registered event handlers */
    std::size_t _nb_invoked_events = 0; /**< Counter for the number of invoked events */

public:
    /**
     * @brief Constructor
     * 
     * Creates a new listener with a dynamic event loop using automatic detection
     * of the best available backend.
     */
    listener()
        : _loop(EVFLAG_AUTO) {}

    /**
     * @brief Clear all registered events
     * 
     * Removes all registered events and runs the event loop once to process
     * any pending events.
     */
    void
    clear() {
        // for (auto it : _registeredEvents)
        //    delete it;
        _registeredEvents.clear();
        run(EVRUN_ONCE);
    }

    /**
     * @brief Destructor
     * 
     * Cleans up by clearing all registered events.
     */
    ~listener() noexcept {
        clear();
    }

    /**
     * @brief Event callback handler
     * 
     * This method is called by libev when an event is triggered. It updates
     * the event's revents field and invokes the associated handler.
     * 
     * @tparam EV_EVENT The event type
     * @param event The event that was triggered
     * @param revents The triggered event flags
     */
    template <typename EV_EVENT>
    void
    on(EV_EVENT &event, int revents) {
        auto &w = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
        w._revents = revents;
        w._interface->invoke();
        ++_nb_invoked_events;
    }

    /**
     * @brief Register an event handler
     * 
     * Creates and registers a new event handler for the specified event type
     * and actor.
     * 
     * @tparam _Event The event type
     * @tparam _Actor The actor (handler) type
     * @tparam _Args Types of additional arguments for event initialization
     * @param actor Reference to the actor that will handle the event
     * @param args Additional arguments for event initialization
     * @return Reference to the created event
     */
    template <typename _Event, typename _Actor, typename... _Args>
    _Event &
    registerEvent(_Actor &actor, _Args &&...args) {
        auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
        revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(
            this);
        revent->_event._interface = revent;

        if constexpr (sizeof...(_Args) > 0)
            revent->_event.set(std::forward<_Args>(args)...);

        _registeredEvents.emplace(revent);
        return revent->_event;
    }

    /**
     * @brief Unregister an event handler
     * 
     * Removes and deletes the specified event handler.
     * 
     * @param kevent Pointer to the event handler to unregister
     */
    void
    unregisterEvent(IRegisteredKernelEvent *kevent) {
        _registeredEvents.erase(kevent);
        delete kevent;
    }

    /**
     * @brief Get the event loop
     * @return Reference to the libev event loop
     */
    [[nodiscard]] inline ev::loop_ref
    loop() const {
        return _loop;
    }

    /**
     * @brief Run the event loop
     * 
     * Executes the event loop with the specified flag.
     * 
     * @param flag The libev run flag (e.g., EVRUN_NOWAIT, EVRUN_ONCE)
     */
    inline void
    run(int flag = 0) {
        _nb_invoked_events = 0;
        _loop.run(flag);
    }

    /**
     * @brief Get the number of invoked events
     * @return The number of events that were invoked in the last run
     */
    [[nodiscard]] inline std::size_t
    nb_invoked_event() const {
        return _nb_invoked_events;
    }

    /**
     * @brief Get the number of registered events
     * @return The number of currently registered events
     */
    [[nodiscard]] inline std::size_t
    size() const {
        return _registeredEvents.size();
    }
};

/**
 * @brief Initialize the asynchronous event system
 * 
 * Clears the current thread's listener, preparing it for use.
 */
inline void
init() {
    listener::current.clear();
}

/**
 * @brief Run the event loop
 * 
 * Executes the current thread's event loop with the specified flag.
 * 
 * @param flag The libev run flag (e.g., EVRUN_NOWAIT, EVRUN_ONCE)
 * @return The number of events that were invoked
 */
inline std::size_t
run(int flag = 0) {
    listener::current.run(flag);
    return listener::current.nb_invoked_event();
}

/**
 * @brief Run the event loop until a condition is met
 * 
 * Repeatedly runs the event loop until the specified status becomes false.
 * 
 * @param status Reference to a boolean condition to check
 */
inline void
run_once() {
    listener::current.run(EVRUN_ONCE);
}

inline void
run_until(bool const &status) {
    while (status)
        listener::current.run(EVRUN_NOWAIT);
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_LISTENER_H_
