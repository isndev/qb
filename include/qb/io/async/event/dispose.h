/**
 * @file qb/io/async/event/dispose.h
 * @brief Resource disposal event for asynchronous I/O components.
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
 * @ingroup AsyncEvent
 */

#ifndef QB_IO_ASYNC_EVENT_DISPOSE_H
#define QB_IO_ASYNC_EVENT_DISPOSE_H

namespace qb::io::async::event {

/**
 * @struct dispose
 * @ingroup AsyncEvent
 * @brief Event triggered just before an asynchronous I/O object is destroyed or cleaned up.
 *
 * This event is passed to the derived class's `on(qb::io::async::event::dispose&&)` method
 * as a final opportunity to perform cleanup operations before the I/O object and its
 * associated resources (like event watchers) are released. This is typically part of the
 * `dispose()` method in base classes like `qb::io::async::io`.
 *
 * Usage Example:
 * @code
 * class MyResourcefulIO : public qb::io::async::io<MyResourcefulIO> {
 * public:
 *   // ... constructor, other methods ...
 *
 *   void on(qb::io::async::event::dispose &&) {
 *     LOG_INFO("MyResourcefulIO disposing: cleaning up custom resources.");
 *     // Perform final cleanup operations specific to MyResourcefulIO
 *     // Note: Base class dispose() will handle listener unregistration.
 *   }
 *
 *   void someErrorCondition() {
 *     // ... error detected ...
 *     this->disconnect(); // This will eventually lead to dispose() being called internally.
 *   }
 * };
 * @endcode
 */
struct dispose {};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_DISPOSE_H
