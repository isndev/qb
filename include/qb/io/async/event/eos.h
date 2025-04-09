/**
 * @file qb/io/async/event/eos.h
 * @brief End-of-stream event for asynchronous I/O
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
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_EVENT_EOS_H
#define QB_IO_ASYNC_EVENT_EOS_H

namespace qb::io::async::event {

/**
 * @struct eos
 * @brief Event triggered when all data has been written and sent
 *
 * This event is passed to the derived class's on() method when
 * all pending data has been successfully written and sent through
 * an I/O stream. It signals that the output buffer is now empty.
 *
 * Usage:
 * @code
 * void on(qb::io::async::event::eos &&) {
 *     // Handle end-of-stream condition
 *     // e.g. close the connection, log completion, etc.
 * }
 * @endcode
 */
struct eos {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EOS_H
