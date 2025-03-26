/**
 * @file qb/io/transport/tcp.h
 * @brief TCP transport implementation for the QB IO library
 * 
 * This file provides a transport implementation for TCP sockets,
 * extending the stream class with TCP-specific functionality.
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
 * @brief TCP transport class
 * 
 * This class implements a transport layer for TCP sockets by extending
 * the generic stream class with TCP-specific socket implementation.
 * It inherits all the stream functionality and applies it to TCP connections.
 */
class tcp : public stream<io::tcp::socket> {
public:
    // Inherits all functionality from the stream class
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_TCP_H
