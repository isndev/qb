/**
 * @file qb/io/async/event/eof.h
 * @brief End-of-file event for asynchronous I/O
 * 
 * This file defines the eof (End-Of-File) event structure which is triggered
 * when there is nothing more to read from an I/O stream. Derived classes can
 * handle this event by implementing the `void on(eof &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_EOF_H
#define QB_IO_ASYNC_EVENT_EOF_H

namespace qb::io::async::event {

/**
 * @struct eof
 * @brief Event triggered when no more data is available for reading
 * 
 * This event is passed to the derived class's on() method when
 * there is nothing more to read from an I/O stream. It signals
 * that the end of the input has been reached.
 * 
 * Usage:
 * @code
 * void on(qb::io::async::event::eof &&) {
 *     // Handle end-of-file condition
 *     // e.g. close the stream, notify users, etc.
 * }
 * @endcode
 */
struct eof {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EOF_H
