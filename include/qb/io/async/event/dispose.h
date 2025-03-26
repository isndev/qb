/**
 * @file qb/io/async/event/dispose.h
 * @brief Resource disposal event for asynchronous I/O
 * 
 * This file defines the dispose event structure which is triggered
 * before an I/O object is destroyed. It allows derived classes to
 * perform cleanup operations by implementing the `void on(dispose &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_DISPOSE_H
#define QB_IO_ASYNC_EVENT_DISPOSE_H

namespace qb::io::async::event {

/**
 * @struct dispose
 * @brief Event triggered before an I/O object is destroyed
 * 
 * This event is passed to the derived class's on() method
 * before the I/O object is destroyed. It provides an opportunity
 * for the derived class to perform cleanup operations.
 * 
 * Usage:
 * @code
 * void on(qb::io::async::event::dispose &&) {
 *     // Perform cleanup operations
 *     // e.g. release resources, close handles, etc.
 * }
 * @endcode
 */
struct dispose {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_DISPOSE_H
