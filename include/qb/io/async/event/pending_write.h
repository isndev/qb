/**
 * @file qb/io/async/event/pending_write.h
 * @brief Event for pending write data in asynchronous I/O
 * 
 * This file defines the pending_write event structure which is triggered
 * to notify about unsent bytes remaining in the write buffer. Derived classes
 * can handle this event by implementing the `void on(pending_write &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_PENDING_WRITE_H
#define QB_IO_ASYNC_EVENT_PENDING_WRITE_H

namespace qb::io::async::event {

/**
 * @struct pending_write
 * @brief Event triggered when unsent data remains in the write buffer
 * 
 * This event is passed to the derived class's on() method to inform about
 * unsent bytes remaining in the write buffer. This can be useful for 
 * monitoring buffer utilization, implementing flow control, or tracking
 * the progress of large data transfers.
 * 
 * Usage:
 * @code
 * void on(qb::io::async::event::pending_write &&event) {
 *     // Handle pending write data notification
 *     auto remaining_bytes = event.bytes;
 *     // Possibly implement backpressure, log metrics, etc.
 * }
 * @endcode
 */
struct pending_write {
    std::size_t bytes; /**< Number of unsent bytes remaining in the write buffer */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_PENDING_WRITE_H
