/**
 * @file qb/io/transport/stcp.h
 * @brief Secure TCP transport implementation for the QB IO library
 * 
 * This file provides a transport implementation for secure (SSL/TLS) TCP sockets,
 * extending the stream class with secure TCP-specific functionality.
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

#ifndef QB_IO_TRANSPORT_STCP_H
#define QB_IO_TRANSPORT_STCP_H
#include "../stream.h"
#include "../tcp/ssl/socket.h"
#include <iostream>
namespace qb::io::transport {

/**
 * @class stcp
 * @brief Secure TCP transport class
 * 
 * This class implements a transport layer for secure (SSL/TLS) TCP sockets by extending
 * the generic stream class with secure TCP-specific implementation. It handles
 * SSL-specific behavior such as managing pending encrypted data in the SSL buffer.
 */
class stcp : public stream<io::tcp::ssl::socket> {
public:
    /**
     * @brief Read data from the secure TCP socket
     * @return Number of bytes read on success, error code on failure
     * 
     * Reads data from the secure socket in chunks, ensuring that any
     * pending data in the SSL buffer is also retrieved. This is important
     * for SSL sockets where decrypted data might be buffered by the
     * SSL implementation even after a socket read.
     */
    [[nodiscard]] int
    read() noexcept {
        static constexpr const std::size_t bucket_read = 8192;

        auto ret = _in.read(_in_buffer.allocate_back(bucket_read), bucket_read);
        if (ret >= 0) {
            _in_buffer.free_back(bucket_read - ret);
            const auto pending = SSL_pending(transport().ssl_handle());
            if (pending) {
                ret += _in.read(_in_buffer.allocate_back(pending), pending);
            }
        }
        return ret;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_STCP_H
