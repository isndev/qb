/**
 * @file qb/io/async/event/timer.h
 * @brief Timer event for asynchronous I/O
 * 
 * This file defines the timer event structure which is used to handle
 * timed operations in the asynchronous I/O system. It wraps libev's timer
 * watcher functionality to provide timeout and periodic callbacks.
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

#ifndef QB_IO_ASYNC_EVENT_TIMER_H
#define QB_IO_ASYNC_EVENT_TIMER_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct timer
 * @brief Event for handling time-based operations
 * 
 * This event extends the base event with ev::timer functionality from libev.
 * It provides the ability to schedule callbacks after a certain delay or at
 * regular intervals.
 * 
 * Usage:
 * @code
 * void on(qb::io::async::event::timer &&event) {
 *     // Handle timer event
 *     // This could be used for periodic tasks, timeouts, etc.
 * }
 * @endcode
 */
struct timer : base<ev::timer> {
    using base_t = base<ev::timer>; /**< Base type alias */

    /**
     * @brief Constructor
     * 
     * @param loop Reference to the libev event loop
     */
    explicit timer(ev::loop_ref loop)
        : base_t(loop) {}
};

/**
 * @typedef timeout
 * @brief Alias for timer to be used in timeout scenarios
 * 
 * This type is functionally identical to timer but provides semantic
 * clarification when used specifically for timeout handling.
 */
using timeout = timer;

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_TIMER_H
