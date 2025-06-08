/**
 * @file qb/io/async/event/signal.h
 * @brief System signal event handler for asynchronous I/O.
 *
 * This file defines the signal event structure which is used to handle
 * system signals (like SIGINT, SIGTERM, etc.) in an asynchronous manner.
 * It wraps libev's signal watcher functionality (`ev::sig`).
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

#ifndef QB_IO_ASYNC_EVENT_SIGNAL_H
#define QB_IO_ASYNC_EVENT_SIGNAL_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct signal
 * @ingroup AsyncEvent
 * @brief Event for handling system signals asynchronously.
 *
 * This template class extends `qb::io::async::event::base<ev::sig>` and thus wraps an `ev::sig`
 * watcher from libev. It is used to watch for specific system signals (e.g., SIGINT, SIGTERM)
 * and trigger callbacks when they occur. The signal to watch is typically specified as a
 * template parameter.
 *
 * @tparam _SIG The signal number to watch (e.g., `SIGINT`, `SIGTERM`).
 *              If `-1` (the default for the specialized version), the signal number must be set
 *              dynamically using the `set()` method of the underlying `ev::sig` watcher.
 *
 * Usage Example:
 * @code
 * #include <csignal> // For SIGINT
 *
 * // Define a handler for SIGINT
 * class InterruptHandler : public qb::io::async::listener::IRegisteredKernelEvent { // Or an actor, etc.
 * public:
 *   qb::io::async::event::signal<SIGINT> sigint_watcher;
 *
 *   InterruptHandler(ev::loop_ref loop) : sigint_watcher(loop) {
 *     // Register this handler with the listener for the sigint_watcher
 *     // In a real scenario, this would be done via listener::current.registerEvent(*this, ... for the watcher)
 *     // For simplicity, assume registration happens elsewhere or use a base class like qb::io::async::io
 *     sigint_watcher.set<&InterruptHandler::on_signal_event_cb>(this); // Simplified libev callback setup
 *     sigint_watcher.start();
 *   }
 *
 *   // This is a simplified libev-style callback, not the qb::io::async::io on() signature
 *   void on_signal_event_cb(ev::sig &watcher, int revents) {
 *     if (watcher.signum == SIGINT) {
 *        LOG_INFO("SIGINT received, shutting down gracefully...");
 *        // application_is_running = false; // Signal main loop to exit
 *        watcher.loop.break_loop(ev::ALL); // Stop the event loop
 *     }
 *   }
 *
 *   // If using qb::io::async::io or similar base, you'd implement:
 *   // void on(qb::io::async::event::signal<SIGINT>& event) {
 *   //   LOG_INFO("SIGINT received via qb event system, signum: " << event.signum);
 *   //   // application_is_running = false;
 *   //   event.loop.break_loop(ev::ALL);
 *   // }
 * };
 * @endcode
 */
template <int _SIG = -1>
struct signal : public base<ev::sig> {
    using base_t = base<ev::sig>; /**< Base type alias for `base<ev::sig>`. */

    /**
     * @brief Constructor.
     *
     * Creates a signal watcher for the specified signal number (if provided as template argument).
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this watcher will be associated with.
     */
    explicit signal(ev::loop_ref loop)
        : base_t(loop) {
        if constexpr (_SIG != -1) {
            this->set(_SIG); // `this->` is needed here for dependent name in template
        }
    }
};

/**
 * @struct signal<-1>
 * @ingroup AsyncEvent
 * @brief Specialization for dynamic signal specification.
 *
 * This specialization of `qb::io::async::event::signal` allows the signal number
 * to be specified dynamically at runtime using the `set()` method of the underlying
 * `ev::sig` watcher, rather than at compile time via a template argument.
 */
template <>
struct signal<-1> : public base<ev::sig> {
    using base_t = base<ev::sig>; /**< Base type alias for `base<ev::sig>`. */

    /**
     * @brief Constructor.
     *
     * Creates a signal watcher without initializing the signal number.
     * The signal must be set later using the `set(int signum)` method of the `ev::sig` watcher.
     * @param loop Reference to the libev event loop (`ev::loop_ref`) this watcher will be associated with.
     */
    explicit signal(ev::loop_ref loop)
        : base_t(loop) {}
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_SIGNAL_H
