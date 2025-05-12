/**
 * @file qb/io/async/event/eof.h
 * @brief End-of-file event for asynchronous input streams.
 *
 * This file defines the eof (End-Of-File) event structure which is triggered
 * when there is nothing more to read from an I/O stream and no partial message remains.
 * Derived classes can handle this event by implementing the `void on(eof &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_EOF_H
#define QB_IO_ASYNC_EVENT_EOF_H

namespace qb::io::async::event {

/**
 * @struct eof
 * @ingroup AsyncEvent
 * @brief Event triggered when no more data is available for reading from an input stream.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::eof&&)` method when
 * an attempt to read from an input I/O object (e.g., TCP socket, file) yields no new data,
 * and the input buffer (after protocol processing) is also empty. It signals that the end
 * of the input stream has been reached or the read operation would block.
 *
 * This is distinct from `qb::io::async::event::disconnected`, which signals a connection closure.
 * An `eof` might occur on a still-open connection if the peer has simply stopped sending data.
 *
 * Usage Example:
 * @code
 * class MyInputHandler : public qb::io::async::input<MyInputHandler> { // Or similar base
 * public:
 *   // ... protocol definition and other methods ...
 *
 *   void on(qb::io::async::event::eof &&) {
 *     LOG_INFO("End of file/stream reached for input.");
 *     // Optionally, close the input stream or take other actions.
 *     // For example, if this is a client, it might decide to disconnect.
 *     // this->disconnect(); 
 *   }
 * };
 * @endcode
 */
struct eof {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EOF_H
