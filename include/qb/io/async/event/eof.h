/**
 * @file qb/io/async/event/eof.h
 * @brief End-of-file event for asynchronous input streams.
 *
 * This file defines the eof (End-Of-File) event structure which is triggered
 * when there is nothing more to read from an I/O stream and no partial message remains.
 * Derived classes can handle this event by implementing the `void on(eof &&)` method.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * @struct input_drained
 * @ingroup AsyncEvent
 * @brief Event triggered when the input buffer has been fully consumed by the protocol.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::input_drained&&)`
 * method by asynchronous input components (`qb::io::async::input` or
 * `qb::io::async::io`) once all complete messages have been parsed from the buffer and
 * `pendingRead()` reports zero bytes remaining.
 *
 * **This is not an end-of-stream notification.** The connection may still be perfectly
 * healthy — the peer has merely paused sending. For an actual connection closure, use
 * `qb::io::async::event::disconnected` instead.
 *
 * @see eof Backward-compatible type alias retained for existing code.
 * @see pending_read For the complementary event indicating leftover bytes in the buffer.
 *
 * Usage Example:
 * @code
 * class MyInputHandler : public qb::io::async::input<MyInputHandler> {
 * public:
 *   void on(qb::io::async::event::input_drained &&) {
 *     LOG_INFO("Input buffer drained; waiting for more data.");
 *   }
 * };
 * @endcode
 */
struct input_drained {};

/**
 * @typedef eof
 * @ingroup AsyncEvent
 * @brief Backward-compatibility alias for @ref input_drained.
 *
 * The historical name suggested end-of-file / end-of-stream semantics, which was
 * misleading: this event fires on any successful read that empties the buffer,
 * even on a still-open connection. New code should prefer @ref input_drained.
 */
using eof = input_drained;

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EOF_H
