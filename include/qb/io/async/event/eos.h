/**
 * @file qb/io/async/event/eos.h
 * @brief End-of-stream event for asynchronous output streams.
 *
 * This file defines the eos (End-Of-Stream) event structure which is triggered
 * when all data has been written and sent through an I/O stream. Derived classes
 * can handle this event by implementing the `void on(eos &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_EOS_H
#define QB_IO_ASYNC_EVENT_EOS_H

namespace qb::io::async::event {

/**
 * @struct eos
 * @ingroup AsyncEvent
 * @brief Event triggered when all buffered data has been successfully written and sent to an output stream.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::eos&&)` method when
 * the output buffer for an I/O object (e.g., TCP socket) becomes empty after a write operation.
 * It signifies that all data previously queued for sending via `publish()` or `operator<<`
 * has been flushed to the underlying transport.
 *
 * This is often used to signal completion of a data transfer or to manage flow control
 * (e.g., stop listening for write readiness on the socket until more data is available).
 *
 * Usage Example:
 * @code
 * class MyOutputHandler : public qb::io::async::output<MyOutputHandler> { // Or similar base
 * public:
 *   // ... other methods ...
 *
 *   void sendLargeData(const char* data, size_t size) {
 *     this->publish(data, size); // Add data to output buffer
 *     // The async::output base will handle writing and trigger eos when done.
 *   }
 *
 *   void on(qb::io::async::event::eos &&) {
 *     LOG_INFO("All pending data has been sent.");
 *     // Optionally, close the connection if this was the last piece of data,
 *     // or notify another component about the completion.
 *     // if (isLastChunk()) {
 *     //   this->disconnect();
 *     // }
 *   }
 * };
 * @endcode
 */
struct eos {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EOS_H
