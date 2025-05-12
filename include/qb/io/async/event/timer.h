/**
 * @file qb/io/async/event/timer.h
 * @brief Timer event for asynchronous I/O and timed operations.
 *
 * This file defines the timer event structure which is used to handle
 * timed operations in the asynchronous I/O system. It wraps libev's timer
 * watcher functionality (`ev::timer`) to provide timeout and periodic callbacks.
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
 * @ingroup AsyncEvent
 */

#ifndef QB_IO_ASYNC_EVENT_TIMER_H
#define QB_IO_ASYNC_EVENT_TIMER_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct timer
 * @ingroup AsyncEvent
 * @brief Event for handling time-based operations (timers and timeouts).
 *
 * This event extends `qb::io::async::event::base<ev::timer>` and thus wraps an `ev::timer`
 * watcher from libev. It provides the ability to schedule callbacks after a certain delay
 * or at regular intervals. It's the foundation for `qb::io::async::with_timeout` and
 * `qb::io::async::callback`.
 *
 * When a handler receives this event, it means the timer has expired.
 * The underlying `ev::timer` can be configured for one-shot or repeating behavior.
 *
 * Usage Example (within a class using `with_timeout` which manages a `timer` event internally):
 * @code
 * class MyTimeoutHandler : public qb::io::async::with_timeout<MyTimeoutHandler> {
 * public:
 *   MyTimeoutHandler(double timeout_seconds) : with_timeout(timeout_seconds) {}
 *
 *   void onActivity() {
 *     updateTimeout(); // Reset the timer on activity
 *   }
 *
 *   void on(qb::io::async::event::timer &&event) { // Or const timer& if not modifying it
 *     LOG_INFO("Timeout occurred!");
 *     // Handle the timeout, e.g., close a connection, retry an operation.
 *     // If it was a one-shot timer, it stops automatically.
 *     // For periodic, call updateTimeout() or set() then start() on the event watcher to re-arm.
 *   }
 * };
 * @endcode
 */
struct timer : base<ev::timer> {
    using base_t = base<ev::timer>; /**< Base type alias for `base<ev::timer>`. */

    /**
     * @brief Constructor.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this timer watcher will be associated with.
     */
    explicit timer(ev::loop_ref loop)
        : base_t(loop) {}
};

/**
 * @typedef timeout
 * @ingroup AsyncEvent
 * @brief Alias for `qb::io::async::event::timer` to be used specifically in timeout scenarios.
 *
 * This type is functionally identical to `qb::io::async::event::timer` but provides semantic
 * clarification when a timer is primarily used for implementing a timeout mechanism.
 */
using timeout = timer;

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_TIMER_H
