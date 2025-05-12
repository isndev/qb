/**
 * @file qb/io/async/event/base.h
 * @brief Base class for asynchronous events in the QB IO library
 *
 * This file defines the base infrastructure for events in the asynchronous I/O system.
 * It provides an interface for kernel event registration and a base template class
 * that wraps libev events to be used throughout the library.
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

#ifndef QB_IO_ASYNC_EVENT_BASE_H
#define QB_IO_ASYNC_EVENT_BASE_H

#include <ev/ev++.h>

namespace qb::io::async {

/**
 * @interface IRegisteredKernelEvent
 * @ingroup Async
 * @brief Interface for kernel event registration and invocation.
 *
 * This interface provides a common abstraction for objects that can be registered
 * with the `listener` to handle specific kernel-level events (wrapped by libev).
 * When a monitored event occurs, the `listener` calls the `invoke()` method
 * of the corresponding `IRegisteredKernelEvent` implementation.
 */
class IRegisteredKernelEvent {
public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~IRegisteredKernelEvent() = default;

    /**
     * @brief Event invocation method, called by the listener when the event triggers.
     *
     * Implementing classes should define their specific event handling logic in this method.
     * This typically involves casting to the concrete event type and calling the user's
     * `on(SpecificEvent&)` handler.
     */
    virtual void invoke() = 0;
};

namespace event {

/**
 * @class base
 * @ingroup AsyncEvent
 * @brief Base template class for all qb-io specific asynchronous event types.
 *
 * This template class serves as the foundation for specific event wrappers like
 * `qb::io::async::event::io`, `qb::io::async::event::timer`, etc. It wraps the
 * corresponding libev event watcher (e.g. `ev::io`, `ev::timer`) and holds a pointer
 * to the `IRegisteredKernelEvent` interface for dispatching.
 *
 * @tparam _EV_EVENT The libev event watcher type (e.g. `ev::io`, `ev::timer`) being wrapped.
 */
template <typename _EV_EVENT>
struct base : public _EV_EVENT {
    using ev_t = _EV_EVENT;             /**< Alias for the underlying libev event watcher type. */
    IRegisteredKernelEvent *_interface; /**< Pointer to the kernel event interface responsible for handling this event. */
    int                     _revents;   /**< Stores the event flags (e.g., EV_READ, EV_WRITE) received from libev when the event triggers. */

    /**
     * @brief Constructor.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this event will be associated with.
     */
    explicit base(ev::loop_ref loop)
        : _EV_EVENT(loop)
        , _interface(nullptr)
        , _revents(0) {}
};

} // namespace event
} // namespace qb::io::async

#endif // QB_IO_ASYNC_EVENT_BASE_H
