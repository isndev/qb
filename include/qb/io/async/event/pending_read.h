/**
 * @file qb/io/async/event/pending_read.h
 * @brief Event for notifying about pending (unprocessed) read data in asynchronous I/O.
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
 * @ingroup AsyncEvent
 */

#ifndef QB_IO_ASYNC_EVENT_PENDINGREAD_H
#define QB_IO_ASYNC_EVENT_PENDINGREAD_H

namespace qb::io::async::event {

/**
 * @struct pending_read
 * @ingroup AsyncEvent
 * @brief Event triggered when unprocessed data remains in the input buffer after protocol processing.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::pending_read&&)` method
 * by some asynchronous input components (like `qb::io::async::input` or `qb::io::async::io`)
 * to inform that after one or more messages were parsed by the protocol, there are still
 * `bytes` remaining in the input buffer. This usually indicates a partial next message.
 *
 * This event is typically informational. The remaining data will stay in the buffer
 * to be combined with subsequent reads. It can be useful for monitoring buffer states
 * or for protocols that might need to take action based on partially received data.
 *
 * Usage Example:
 * @code
 * class MyProtocolHandler : public qb::io::async::input<MyProtocolHandler> {
 * public:
 *   // ... protocol definition and on(ProtocolMessage&) handler ...
 *
 *   void on(qb::io::async::event::pending_read &&event) {
 *     LOG_DEBUG("Pending read data: " << event.bytes << " bytes remaining in input buffer.");
 *     // Typically no action is needed here as the data remains for the next read cycle.
 *     // However, one might implement logic for very large partial messages if necessary.
 *   }
 * };
 * @endcode
 */
struct pending_read {
    std::size_t bytes; /**< Number of unprocessed bytes remaining in the read buffer
                           *  after successful protocol message extraction. */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_PENDINGREAD_H
