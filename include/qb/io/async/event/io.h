/**
 * @file qb/io/async/event/io.h
 * @brief I/O notification event for asynchronous operations
 * 
 * This file defines the io event structure which is used internally by the library
 * to handle low-level I/O notifications from libev. It wraps libev's io watcher
 * functionality for read/write readiness on file descriptors.
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

#ifndef QB_IO_ASYNC_EVENT_IO_H
#define QB_IO_ASYNC_EVENT_IO_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct io
 * @brief Event for file descriptor I/O notifications
 * 
 * This event extends the base event with ev::io functionality from libev.
 * It is used internally by the I/O subsystem to monitor file descriptors
 * for read/write readiness and other I/O conditions.
 * 
 * Note: This event is primarily used internally by the library and is not
 * typically meant to be handled directly by user code.
 * 
 * @note This event wrapper is used internally by the I/O subsystem
 * and is not typically meant to be handled directly by application code.
 */
struct io : base<ev::io> {
    using base_t = base<ev::io>; /**< Base type alias */

    /**
     * @brief Constructor
     * 
     * @param loop Reference to the libev event loop
     */
    explicit io(ev::loop_ref loop)
        : base_t(loop) {}
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_IO_H
