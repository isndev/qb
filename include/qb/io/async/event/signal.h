/**
 * @file qb/io/async/event/signal.h
 * @brief System signal event handler for asynchronous I/O
 * 
 * This file defines the signal event structure which is used to handle
 * system signals (like SIGINT, SIGTERM, etc.) in an asynchronous manner.
 * It wraps libev's signal watcher functionality.
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

#ifndef QB_IO_ASYNC_EVENT_SIGNAL_H
#define QB_IO_ASYNC_EVENT_SIGNAL_H

#include "base.h"

namespace qb::io::async::event {

/**
 * @struct signal
 * @brief Event for handling system signals
 * 
 * This template class extends the base event with ev::sig functionality from libev.
 * It is used to watch for specific system signals and trigger callbacks when they occur.
 * The template parameter allows specifying which signal to watch at compile time.
 * 
 * @tparam _SIG The signal number to watch, or -1 for dynamic signal specification
 * 
 * Usage:
 * @code
 * // Create a SIGINT handler
 * using sigint_handler = qb::io::async::event::signal<SIGINT>;
 * 
 * // In the derived class:
 * void on(sigint_handler &&sig) {
 *     // Handle SIGINT signal
 * }
 * @endcode
 */
template <int _SIG = -1>
struct signal : public base<ev::sig> {
    using base_t = base<ev::sig>; /**< Base type alias */

    /**
     * @brief Constructor
     * 
     * Creates a signal watcher for the specified signal.
     * 
     * @param loop Reference to the libev event loop
     */
    explicit signal(ev::loop_ref loop)
        : base_t(loop) {
        set(_SIG);
    }
};

/**
 * @struct signal<-1>
 * @brief Specialization for dynamic signal specification
 * 
 * This specialization allows the signal to be specified dynamically
 * rather than at compile time.
 */
template <>
struct signal<-1> : public base<ev::sig> {
    using base_t = base<ev::sig>; /**< Base type alias */

    /**
     * @brief Constructor
     * 
     * Creates a signal watcher without initializing the signal number.
     * The signal must be set later using the set() method.
     * 
     * @param loop Reference to the libev event loop
     */
    explicit signal(ev::loop_ref loop)
        : base_t(loop) {}
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_SIGNAL_H
