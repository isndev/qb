/**
 * @file qb/io/async/event/disconnected.h
 * @brief Disconnection event for asynchronous I/O
 *
 * This file defines the disconnected event structure which is triggered
 * when an I/O connection is closed or lost. The derived classes can handle
 * this event by implementing the `void on(disconnected &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_DISCONNECTED_H
#define QB_IO_ASYNC_EVENT_DISCONNECTED_H

namespace qb::io::async::event {

/**
 * @struct disconnected
 * @brief Event triggered when a connection is closed or lost
 *
 * This event is passed to the derived class's on() method when
 * a disconnection occurs in an I/O object. The reason field can
 * contain a code indicating the reason for disconnection.
 *
 * Usage:
 * @code
 * void on(qb::io::async::event::disconnected &&event) {
 *     // Handle disconnection event
 *     int reason = event.reason;
 * }
 * @endcode
 */
struct disconnected {
    int reason = 0; /**< Reason code for the disconnection (0 = normal, others may
                       indicate errors) */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_DISCONNECTED_H
