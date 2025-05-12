/**
 * @file qb/io/transport/tcp.h
 * @brief TCP stream transport implementation for the QB IO library.
 *
 * This file provides a transport implementation for TCP sockets,
 * extending the `qb::io::stream` class with `qb::io::tcp::socket` specific functionality
 * for reliable, stream-oriented network communication.
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
 * @ingroup TCP
 */

#ifndef QB_IO_TRANSPORT_TCP_H
#define QB_IO_TRANSPORT_TCP_H
#include "../stream.h"
#include "../tcp/socket.h"

namespace qb::io::transport {

/**
 * @class tcp
 * @ingroup Transport
 * @brief TCP transport providing reliable, stream-based network communication.
 *
 * This class implements a transport layer for TCP sockets by extending
 * the generic `qb::io::stream` class, specializing it with `qb::io::tcp::socket`
 * as the underlying I/O mechanism. It inherits all stream functionality for buffered
 * reading and writing and applies it to TCP connections (IPv4, IPv6, or Unix Sockets).
 */
class tcp : public stream<io::tcp::socket> {
public:
    // Inherits all functionality from the stream class
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_TCP_H
