/**
 * @file qb/io/async/event/pending_write.h
 * @brief Event for notifying about pending (unsent) write data in asynchronous I/O.
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
 * @ingroup AsyncEvent
 */

#ifndef QB_IO_ASYNC_EVENT_PENDING_WRITE_H
#define QB_IO_ASYNC_EVENT_PENDING_WRITE_H

namespace qb::io::async::event {

/**
 * @struct pending_write
 * @ingroup AsyncEvent
 * @brief Event triggered when unsent data remains in the output buffer after a write operation.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::pending_write&&)` method
 * by some asynchronous output components (like `qb::io::async::output` or `qb::io::async::io`)
 * if a `write()` operation to the underlying transport did not send all the data currently
 * in the output buffer (e.g., because the socket's send buffer was full).
 *
 * The `bytes` member indicates how much data is still pending. The I/O component will typically
 * continue to listen for write readiness and attempt to send the remaining data.
 * This event is useful for monitoring buffer utilization, implementing flow control (e.g., pausing
 * data production if the buffer grows too large), or tracking the progress of large data transfers.
 *
 * Usage Example:
 * @code
 * class MyDataSender : public qb::io::async::output<MyDataSender> {
 * public:
 *   // ... other methods ...
 *
 *   void on(qb::io::async::event::pending_write &&event) {
 *     LOG_DEBUG("Pending write data: " << event.bytes << " bytes still in output buffer.");
 *     if (event.bytes > HIGH_WATER_MARK) {
 *       // pauseProducingData(); // Example of flow control
 *     }
 *   }
 *
 *   void on(qb::io::async::event::eos &&) { // End Of Stream
 *      LOG_DEBUG("All data sent, output buffer empty.");
 *      // resumeProducingData();
 *   }
 * };
 * @endcode
 */
struct pending_write {
    std::size_t bytes; /**< Number of unsent bytes remaining in the write buffer
                           *  after a partial write operation. */
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_PENDING_WRITE_H
