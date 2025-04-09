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
 * @brief Interface for kernel event registration
 *
 * This interface provides a common abstraction for event registration
 * in the event loop system. Classes implementing this interface can be
 * invoked when the corresponding event occurs.
 */
class IRegisteredKernelEvent {
public:
    /**
     * @brief Virtual destructor
     */
    virtual ~IRegisteredKernelEvent() = default;

    /**
     * @brief Event invocation method
     *
     * Called when the registered event is triggered. Implementing classes
     * should define their specific event handling logic in this method.
     */
    virtual void invoke() = 0;
};

namespace event {

/**
 * @class base
 * @brief Base template class for all event types
 *
 * This template class serves as the foundation for all event types in the system.
 * It wraps libev events and provides a connection to the registered kernel event
 * interface.
 *
 * @tparam _EV_EVENT The libev event type to wrap (e.g. ev::io, ev::timer)
 */
template <typename _EV_EVENT>
struct base : public _EV_EVENT {
    using ev_t = _EV_EVENT;             /**< The underlying libev event type */
    IRegisteredKernelEvent *_interface; /**< Pointer to the kernel event interface */
    int                     _revents;   /**< Event flags received from libev */

    /**
     * @brief Constructor
     *
     * @param loop Reference to the libev event loop
     */
    explicit base(ev::loop_ref loop)
        : _EV_EVENT(loop)
        , _interface(nullptr)
        , _revents(0) {}
};

} // namespace event
} // namespace qb::io::async

#endif // QB_IO_ASYNC_EVENT_BASE_H
