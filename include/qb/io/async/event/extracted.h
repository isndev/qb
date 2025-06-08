/**
 * @file qb/io/async/event/extracted.h
 * @brief Extracted event for asynchronous I/O operations.
 *
 * This file defines the extracted event structure which is triggered
 * when an I/O connection is extracted from an I/O object. The derived classes can handle
 * this event by implementing the `void on(extracted &&)` method.
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

#ifndef QB_IO_ASYNC_EVENT_EXTRACTED_H
#define QB_IO_ASYNC_EVENT_EXTRACTED_H

namespace qb::io::async::event {

/**
 * @struct extracted
 * @ingroup AsyncEvent
 * @brief Event triggered when a connection is extracted from an I/O object.
 *
 * This event is passed to the derived class's `on()` method when
 * a connection is extracted from an I/O object.
 *
 * Usage Example:
 * @code
 * class MyNetworkHandler : public qb::io::async::io<MyNetworkHandler> { // Or similar base
 * public:
 *   // ... other methods ...
 *   void on(qb::io::async::event::extracted &&event) {
 *     // Perform cleanup, attempt reconnection, etc.
 *   }
 * };
 * @endcode
 */
struct extracted {
};

} // namespace qb::io::async::event

#endif // QB_IO_ASYNC_EVENT_EXTRACTED_H
