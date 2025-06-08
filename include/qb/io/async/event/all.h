/**
 * @file qb/io/async/event/all.h
 * @brief Aggregation of all event types for the asynchronous I/O system
 *
 * This file includes all the event types defined in the qb::io::async::event
 * namespace. Including this file gives access to the complete set of events
 * that can be used with the asynchronous I/O system.
 *
 * Event handling in QB IO is based on the observer pattern. Classes derived
 * from io/input/output can receive event notifications by implementing
 * handlers with the signature: void on(event_type &&);
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

#ifndef QB_IO_ASYNC_EVENT_ALL_H
#define QB_IO_ASYNC_EVENT_ALL_H

#include "disconnected.h"  // Event for connection loss
#include "extracted.h"     // Event for connection extraction
#include "dispose.h"       // Resource disposal event
#include "eof.h"           // End-of-file event
#include "eos.h"           // End-of-stream event
#include "file.h"          // Event for file/directory changes
#include "io.h"            // Low-level I/O event
#include "pending_read.h"  // Event for unprocessed read data
#include "pending_write.h" // Event for unsent write data
#include "signal.h"        // System signal event
#include "timer.h"         // Timer/timeout event
#include "handshake.h"     // Handshake event

#endif // QB_IO_ASYNC_EVENT_ALL_H
