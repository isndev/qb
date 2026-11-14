/**
 * @file qb/io/async/event/io.h
 * @brief Low-level I/O notification event for asynchronous operations.
 *
 * This file defines the io event structure which is used internally by the library
 * to handle low-level I/O notifications from libev. It wraps libev's io watcher
 * functionality for read/write readiness on file descriptors.
 * This event is fundamental for most asynchronous socket and file operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#ifndef QB_IO_ASYNC_EVENT_IO_H
#define QB_IO_ASYNC_EVENT_IO_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct io
 * @ingroup AsyncEvent
 * @brief Event for file descriptor I/O readiness notifications (read/write).
 *
 * This event extends `qb::io::async::event::base<ev::io>` and thus wraps an `ev::io`
 * watcher from libev. It is used by asynchronous I/O components (like those derived
 * from `qb::io::async::input`, `output`, or `io`) to monitor file descriptors
 * (e.g., sockets) for readiness to perform read or write operations without blocking.
 *
 * The `_revents` member of this struct (inherited from `event::base`) will contain
 * flags like `EV_READ` or `EV_WRITE` indicating the type of readiness.
 *
 * @note This event is primarily used internally by the `qb-io` asynchronous framework
 *       (e.g., within `qb::io::async::io::on(event::io const &event)`). Application code
 *       typically handles higher-level events like message arrival or disconnection rather
 *       than directly processing this raw I/O readiness event.
 */
struct io : base<ev::io> {
    using base_t = base<ev::io>; /**< Base type alias for `base<ev::io>`. */

    /**
     * @brief Constructor: the watcher is born on no descriptor, watching nothing.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this event watcher will be associated with.
     * @details libev leaves the descriptor and the event mask of a new watcher indeterminate until its first `set()`
     *          (`ev_init()` sets neither), and the asynchronous components read that mask as what they watch
     *          (`is_reading()`, `is_writing()`, `ready_to_read()`, `ready_to_write()`) before their first `start()`
     *          too: an `output` that publishes before it starts asks `ready_to_write()`. Read from memory that held
     *          other bytes, the mask answered for a watcher that watched nothing, armed nothing, or armed an illegal
     *          mask, which a build that keeps the loop's assertions aborts on. So both are set here (Huly QB-1270).
     */
    explicit io(ev::loop_ref loop) noexcept
        : base_t(loop) {
        this->set(-1, EV_NONE);
    }
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_IO_H
