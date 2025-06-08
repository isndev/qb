/**
 * @file qb/io/async/event/handshake.h
 * @brief Handshake event for asynchronous input streams.
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

#ifndef QB_IO_ASYNC_EVENT_HANDSHAKE_H
#define QB_IO_ASYNC_EVENT_HANDSHAKE_H

namespace qb::io::async::event {

/**
 * @struct handshake
 * @ingroup AsyncEvent
 * @brief Event triggered when the handshake is complete.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::handshake&&)` method when
 * the handshake is complete.
 *
 * Usage Example:
 * @code
 * class MyIO : public qb::io::async::io<MyIO> {
 * public:
 *   // ... protocol definition and other methods ...
 *
 *   void on(qb::io::async::event::handshake &&) {
 *     LOG_INFO("Handshake complete.");
 *     // Optionally, close the input stream or take other actions.
 *     // For example, if this is a client, it might decide to disconnect.
 *     // this->disconnect(); 
 *   }
 * };
 * @endcode
 */
struct handshake {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_HANDSHAKE_H
