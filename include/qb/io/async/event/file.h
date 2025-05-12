/**
 * @file qb/io/async/event/file.h
 * @brief File system monitoring event for asynchronous I/O.
 *
 * This file defines the file event structure which is used by the `qb::io::async::file_watcher`
 * (and `directory_watcher`) to notify about file or directory attribute changes.
 * It wraps libev's stat watcher functionality (`ev::stat`). Derived classes can handle this
 * event by implementing the `void on(file &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_FILE_H
#define QB_IO_ASYNC_EVENT_FILE_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct file
 * @ingroup AsyncEvent
 * @brief Event for file and directory attribute change monitoring.
 *
 * This event extends `qb::io::async::event::base<ev::stat>` and thus wraps an `ev::stat`
 * watcher from libev. It is used by components like `qb::io::async::file_watcher` and
 * `qb::io::async::directory_watcher` to notify when attributes of a monitored file or
 * directory (e.g., size, modification time) have changed.
 *
 * The `ev::stat` member (accessible as `this->attr` or `event.attr` in the handler)
 * contains the current stat attributes, and `event.prev` contains the attributes from
 * the previous check.
 *
 * Usage Example:
 * @code
 * class MyFileMonitor : public qb::io::async::file_watcher<MyFileMonitor> {
 * public:
 *   MyFileMonitor(const std::string& path_to_watch, double interval = 1.0) {
 *     this->start(path_to_watch, interval);
 *   }
 *
 *   void on(qb::io::async::event::file &&event) {
 *     if (event.attr.st_nlink == 0) {
 *       LOG_INFO("File " << this->path() << " deleted or moved.");
 *       this->disconnect(); // Stop watching
 *       return;
 *     }
 *     if (event.attr.st_mtime != event.prev.st_mtime) {
 *       LOG_INFO("File " << this->path() << " modified. New size: " << event.attr.st_size);
 *       // Process file content if needed (e.g., using this->read_all())
 *     }
 *     // Access other stat information through event.attr and event.prev (ev_statdata)
 *   }
 * };
 * @endcode
 */
struct file : base<ev::stat> {
    using base_t = base<ev::stat>; /**< Base type alias for `base<ev::stat>`. */

    /**
     * @brief Constructor.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this event watcher will be associated with.
     */
    explicit file(ev::loop_ref loop)
        : base_t(loop) {}
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_FILE_H
