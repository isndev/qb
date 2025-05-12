/**
 * @file qb/io/async/event/disconnected.h
 * @brief Disconnection event for asynchronous I/O operations.
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
 * @ingroup AsyncEvent
 */

#ifndef QB_IO_ASYNC_EVENT_DISCONNECTED_H
#define QB_IO_ASYNC_EVENT_DISCONNECTED_H

namespace qb::io::async::event {

/**
 * @struct disconnected
 * @ingroup AsyncEvent
 * @brief Event triggered when a connection is closed or lost.
 *
 * This event is passed to the derived class's `on()` method when
 * a disconnection occurs in an I/O object (e.g., a TCP session).
 * The `reason` field can contain a code indicating the cause of disconnection.
 *
 * Usage Example:
 * @code
 * class MyNetworkHandler : public qb::io::async::io<MyNetworkHandler> { // Or similar base
 * public:
 *   // ... other methods ...
 *   void on(qb::io::async::event::disconnected &&event) {
 *     if (event.reason == 0) {
 *       // Normal disconnection by peer or self
 *       LOG_INFO("Connection closed normally.");
 *     } else {
 *       // Disconnection due to an error, event.reason might hold system errno
 *       LOG_WARN("Connection lost, reason: " << event.reason);
 *     }
 *     // Perform cleanup, attempt reconnection, etc.
 *   }
 * };
 * @endcode
 */
struct disconnected {
    int reason = 0; /**< Reason code for the disconnection. Typically `0` for a normal shutdown
                       * initiated by `disconnect()` or peer closing gracefully. Non-zero values
                       * often correspond to system error codes (errno) if the disconnection
                       * was due to an error detected by the underlying transport or OS. */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_DISCONNECTED_H
