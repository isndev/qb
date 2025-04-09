/**
 * @file qb/io/async/event/pending_read.h
 * @brief Event for pending read data in asynchronous I/O
 *
 * This file defines the pending_read event structure which is triggered
 * to notify about unprocessed bytes remaining in the read buffer after
 * protocol message processing. Derived classes can handle this event by
 * implementing the `void on(pending_read &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_PENDINGREAD_H
#define QB_IO_ASYNC_EVENT_PENDINGREAD_H

namespace qb::io::async::event {

/**
 * @struct pending_read
 * @brief Event triggered when unprocessed data remains in the read buffer
 *
 * This event is passed to the derived class's on() method to inform about
 * unprocessed bytes remaining in the read buffer after the protocol has
 * finished processing messages. This can be useful for monitoring buffer
 * utilization or implementing custom buffer management.
 *
 * Usage:
 * @code
 * void on(qb::io::async::event::pending_read &&event) {
 *     // Handle pending read data notification
 *     auto remaining_bytes = event.bytes;
 *     // Possibly adjust buffer sizes, log metrics, etc.
 * }
 * @endcode
 */
struct pending_read {
    std::size_t bytes; /**< Number of unprocessed bytes remaining in the read buffer */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_PENDINGREAD_H
