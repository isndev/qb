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
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_LISTENER_H_
#define QB_IO_ASYNC_LISTENER_H_

#include <algorithm>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <vector>
#include "event/base.h"

namespace qb::io::async {

/**
 * @class listener
 * @ingroup Async
 * @brief Central event loop manager for asynchronous IO operations.
 *
 * The listener class is the core of the asynchronous event system. It manages
 * an event loop (based on libev) that handles all asynchronous events (IO, timer, signal, etc.)
 * and dispatches them to registered handlers.
 *
 * Each thread has its own listener instance accessible via the thread_local
 * static member 'current'.
 */
class listener {
public:
    /**
     * @brief Thread-local instance of the listener.
     *
     * Each thread has its own listener accessible through this static member.
     * This provides a way to access the current thread's event loop without
     * passing a reference explicitly.
     * @note This is typically initialized automatically when `qb::Main` starts its `VirtualCore` threads,
     *       or by calling `qb::io::async::init()` for standalone `qb-io` usage.
     */
    thread_local static listener current;

    /**
     * @class RegisteredKernelEvent
     * @ingroup AsyncEvent
     * @brief Template wrapper for concrete event handlers and their associated libev watchers.
     *
     * This internal class wraps a specific libev event watcher (like `ev::io` or `ev::timer`)
     * and the user-provided actor/handler object. It implements the `IRegisteredKernelEvent`
     * interface.
     *
     * @tparam _Event The event type
     * @tparam _Actor The actor (handler) type
     */
    template <typename _Event, typename _Actor>
    class RegisteredKernelEvent final : public IRegisteredKernelEvent {
        friend class listener;

        _Actor &_actor; /**< Reference to the actor that handles the event */
        _Event  _event; /**< The event object */

        /**
         * @brief Destructor
         */
        ~RegisteredKernelEvent() final {
            _event.stop();
        }

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
    ev::dynamic_loop _loop; /**< The libev event loop */
    qb::unordered_set<IRegisteredKernelEvent *>
                _registeredEvents;      /**< Set of registered event handlers */
    std::size_t _nb_invoked_events = 0; /**< Counter for the number of invoked events */

public:
    /**
     * @brief Constructor
     *
     * Creates a new listener with a dynamic event loop using automatic detection
     * of the best available backend (e.g., epoll, kqueue, select) via libev's EVFLAG_AUTO.
     */
    listener()
        : _loop(EVFLAG_AUTO) {}

    /**
     * @brief Clear all registered events from this listener.
     *
     * Removes and deletes all registered event handlers (IRegisteredKernelEvent instances).
     * It also runs the event loop once with `EVRUN_ONCE` to process any pending libev events
     * before fully clearing, which might be necessary for proper cleanup of some watchers.
     * @note This is automatically called by the listener's destructor.
     */
    void
    clear() {
        if (!_registeredEvents.empty()) {
            for (auto it : _registeredEvents)
                delete it;
            _registeredEvents.clear();
            run(EVRUN_ONCE);
        }
    }

    /**
     * @brief Destructor
     *
     * Cleans up by calling `clear()` to remove all registered events and their watchers.
     */
    ~listener() noexcept {
        clear();
    }

    /**
     * @brief Generic event callback handler invoked by libev for any active watcher.
     *
     * This method is the entry point for libev to notify of an event. It updates
     * the custom event wrapper's `_revents` field and then invokes the stored
     * `IRegisteredKernelEvent::invoke()` method, which in turn calls the user-defined
     * `on(SpecificEvent&)` handler in the registered actor/object.
     *
     * @tparam EV_EVENT The specific libev watcher type (e.g., `ev::io`, `ev::timer`).
     * @param event The libev watcher that was triggered.
     * @param revents The bitmask of triggered event flags (e.g., `EV_READ`, `EV_WRITE`).
     */
    template <typename EV_EVENT>
    void
    on(EV_EVENT &event, int revents) {
        auto &w    = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
        w._revents = revents;
        w._interface->invoke();
        ++_nb_invoked_events;
    }

    /**
     * @brief Register an event handler (actor/object) for a specific asynchronous event type.
     *
     * Creates a `RegisteredKernelEvent` wrapper for the given actor and event type,
     * initializes the underlying libev watcher with the provided arguments, and registers
     * it with this listener's event loop.
     *
     * @tparam _Event The qb-io event type (e.g., `qb::io::async::event::io`, `qb::io::async::event::timer`).
     *                This type wraps a specific libev watcher.
     * @tparam _Actor The type of the class that will handle the event (must have an `on(_Event&)` method).
     * @tparam _Args Types of additional arguments for initializing the libev watcher (e.g., fd and event flags for `ev::io`).
     * @param actor Reference to the actor/object instance that will handle the event.
     * @param args Additional arguments forwarded to the libev watcher's `set()` or equivalent initialization method.
     * @return Reference to the created `_Event` object (which is also the libev watcher).
     *         This reference can be used to later `start()` or `stop()` the watcher.
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
     * @brief Unregister an event handler and its associated libev watcher.
     *
     * Removes the specified `IRegisteredKernelEvent` from the listener's tracking
     * and deletes the event handler object, which also stops and cleans up the
     * underlying libev watcher via its destructor.
     *
     * @param kevent Pointer to the `IRegisteredKernelEvent` to unregister. This pointer
     *               is typically obtained when the event was initially registered or stored
     *               within the libev watcher wrapper itself (e.g., `_Event::_interface`).
     */
    void
    unregisterEvent(IRegisteredKernelEvent *kevent) {
        _registeredEvents.erase(kevent);
        delete kevent;
    }

    /**
     * @brief Get a reference to the underlying libev event loop.
     * @return `ev::loop_ref` (a reference wrapper to `ev_loop*`) for this listener.
     * @details Useful for advanced direct interaction with libev if needed, though most
     *          operations are handled through the listener's API.
     */
    [[nodiscard]] inline ev::loop_ref
    loop() const {
        return _loop;
    }

    /**
     * @brief Run the event loop to process pending events.
     *
     * Executes the event loop with the specified libev run flag.
     * This call blocks or returns based on the flag and event activity.
     * It also resets the `_nb_invoked_events` counter before running.
     *
     * @param flag The libev run flag (e.g., `EVRUN_NOWAIT` to check once and return,
     *             `EVRUN_ONCE` to wait for and process one event block, `0` for default blocking run).
     *             Default is `0`, which means `ev_run` will block until `ev_break` is called or no active watchers remain.
     */
    inline void
    run(int flag = 0) {
        _nb_invoked_events = 0;
        _loop.run(flag);
    }

    /**
     * @brief Request the event loop to break out of its current `run()` cycle.
     * @details This signals the libev loop to stop processing further events in the current
     *          `run()` invocation. If `run()` was called with default blocking behavior,
     *          it will return after the current event (if any) is processed.
     */
    inline void
    break_one() {
        _loop.break_loop();
    }

    /**
     * @brief Get the number of events invoked during the last call to `run()`.
     * @return The count of events that were processed and dispatched to handlers.
     * @note This counter is reset at the beginning of each `run()` call.
     */
    [[nodiscard]] inline std::size_t
    nb_invoked_event() const {
        return _nb_invoked_events;
    }

    /**
     * @brief Get the number of currently registered event handlers.
     * @return The total number of active event watchers managed by this listener.
     */
    [[nodiscard]] inline std::size_t
    size() const {
        return _registeredEvents.size();
    }
};

/**
 * @brief Initialize the asynchronous event system for the current thread.
 * @details Ensures that `listener::current` is available and ready for use.
 *          Typically called once per thread that will use `qb-io` asynchronous features standalone.
 *          Not usually needed when using `qb-core` as `qb::Main` handles this for its `VirtualCore` threads.
 * @ingroup Async
 */
inline void
init() {
    // listener::current.clear();
}

/**
 * @brief Run the event loop for the current thread.
 *
 * Executes the current thread's `listener::current.run(flag)`.
 * This is the primary way to process asynchronous events in a standalone `qb-io` application.
 *
 * @param flag The libev run flag (e.g., `EVRUN_NOWAIT`, `EVRUN_ONCE`). See `listener::run()`.
 * @return The number of events that were invoked during this run.
 * @ingroup Async
 */
inline std::size_t
run(int flag = 0) {
    listener::current.run(flag);
    return listener::current.nb_invoked_event();
}

/**
 * @brief Run the event loop once for the current thread, waiting for at least one event.
 *
 * Equivalent to `listener::current.run(EVRUN_ONCE)`.
 * @return The number of events invoked.
 * @ingroup Async
 */
inline std::size_t
run_once() {
    listener::current.run(EVRUN_ONCE);
    return listener::current.nb_invoked_event();
}

/**
 * @brief Run the event loop for the current thread until a condition is met.
 *
 * Repeatedly calls `listener::current.run(EVRUN_NOWAIT)` as long as the `status` is true.
 * @param status Reference to a boolean condition. The loop continues as long as `status` is true.
 * @return The total number of events invoked across all `run(EVRUN_NOWAIT)` calls.
 * @ingroup Async
 */
inline std::size_t
run_until(bool const &status) {
    std::size_t nb_invoked_events = 0;
    while (status) {
        listener::current.run(EVRUN_NOWAIT);
        nb_invoked_events += listener::current.nb_invoked_event();
    }
    return nb_invoked_events;
}

/**
 * @brief Request the parent (current thread's) event loop to break.
 * @details Calls `listener::current.break_one()`.
 * @ingroup Async
 */
inline void
break_parent() {
    listener::current.break_one();
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_LISTENER_H_
