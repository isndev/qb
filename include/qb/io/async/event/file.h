/**
 * @file qb/io/async/event/file.h
 * @brief File monitoring event for asynchronous I/O
 *
 * This file defines the file event structure which is used by the directory_watcher
 * to notify about file or directory updates. It wraps libev's stat watcher
 * functionality. Derived classes can handle this event by implementing the `void on(file
 * &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_FILE_H
#define QB_IO_ASYNC_EVENT_FILE_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct file
 * @brief Event for file and directory monitoring
 *
 * This event extends the base event with ev::stat functionality from libev.
 * It is used primarily by the directory_watcher component to notify when
 * monitored files or directories are modified.
 *
 * Usage:
 * @code
 * void on(qb::io::async::event::file &&event) {
 *     // Handle file change notification
 *     // Access stat information through libev methods
 * }
 * @endcode
 */
struct file : base<ev::stat> {
    using base_t = base<ev::stat>; /**< Base type alias */

    /**
     * @brief Constructor
     *
     * @param loop Reference to the libev event loop
     */
    explicit file(ev::loop_ref loop)
        : base_t(loop) {}
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_FILE_H
