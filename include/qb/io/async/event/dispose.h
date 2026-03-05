/**
 * @file qb/io/async/event/dispose.h
 * @brief Resource disposal event for asynchronous I/O components.
 *
 * This file defines the dispose event structure which is triggered
 * before an I/O object is destroyed. It allows derived classes to
 * perform cleanup operations by implementing the `void on(dispose &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_DISPOSE_H
#define QB_IO_ASYNC_EVENT_DISPOSE_H

namespace qb::io::async::event {

/**
 * @struct dispose
 * @ingroup AsyncEvent
 * @brief Event triggered just before an asynchronous I/O object is destroyed or cleaned up.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::dispose&&)` method
 * as a final opportunity to perform cleanup operations before the I/O object and its
 * associated resources (like event watchers) are released. This is typically part of the
 * `dispose()` method in base classes like `qb::io::async::io`.
 *
 * @note **Actor Lifecycle Integration:** When used within a `qb::Actor`, this event is triggered
 *       during the I/O component's cleanup phase, which occurs before the actor's destructor.
 *       This allows actors to perform final cleanup of I/O-related resources. The sequence is:
 *       1. `on(event::disconnected&)` is called (if connection was active)
 *       2. `on(event::dispose&)` is called (final cleanup hook)
 *       3. I/O component destructor runs (unregisters from listener)
 *       4. Actor destructor runs (if actor is being destroyed)
 *
 * Usage Example:
 * @code
 * class MyClientActor : public qb::Actor, public qb::io::use<MyClientActor>::tcp::client<MyProtocol> {
 * public:
 *   bool onInit() override {
 *     registerEvent<qb::io::async::event::disconnected>(*this);
 *     registerEvent<qb::io::async::event::dispose>(*this);
 *     registerEvent<qb::KillEvent>(*this);
 *     // ... start connection ...
 *     return true;
 *   }
 *
 *   void on(qb::io::async::event::disconnected const& event) {
 *     LOG_INFO("Connection lost: " << event.error_code.message());
 *     // Optionally attempt reconnection or notify other actors
 *   }
 *
 *   void on(qb::io::async::event::dispose &&) {
 *     LOG_INFO("I/O component disposing: cleaning up custom resources.");
 *     // Perform final cleanup operations specific to MyClientActor
 *     // Note: Base class dispose() will handle listener unregistration.
 *   }
 *
 *   void on(qb::KillEvent const&) {
 *     this->disconnect(); // Gracefully close connection before termination
 *     kill(); // Signal framework to proceed with termination
 *   }
 * };
 * @endcode
 */
struct dispose {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_DISPOSE_H
