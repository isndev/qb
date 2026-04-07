/**
 * @file qb/io/transport/stcp.h
 * @brief Secure TCP (SSL/TLS) stream transport for the QB IO library.
 *
 * This file provides a transport implementation for secure (SSL/TLS) TCP sockets,
 * extending the `qb::io::stream` class with `qb::io::tcp::ssl::socket` specific functionality.
 * It handles encrypted stream-based communication.
 * Requires OpenSSL.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TRANSPORT_STCP_H
#define QB_IO_TRANSPORT_STCP_H
#include "../config.h"
#include "../stream.h"
#include "../tcp/ssl/socket.h"
namespace qb::io::transport {

/**
 * @class stcp
 * @ingroup Transport
 * @brief Secure TCP (SSL/TLS) transport providing encrypted stream communication.
 *
 * This class implements a transport layer for secure (SSL/TLS) TCP sockets by extending
 * the generic `qb::io::stream` class, specializing it with `qb::io::tcp::ssl::socket`
 * as the underlying I/O mechanism. It handles SSL-specific behavior such as managing
 * pending encrypted data in the SSL buffer during read operations to ensure all application
 * data is retrieved.
 */
class stcp : public stream<io::tcp::ssl::socket> {
public:
    /** @brief Indicates that this transport implementation is secure */
    static constexpr bool is_secure() noexcept { return true; }
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
        static constexpr const std::size_t bucket_read = QB_DEFAULT_READ_BUFFER_SIZE;

        if (this->_max_read_buffer_size < _in_buffer.size() ||
            bucket_read > this->_max_read_buffer_size - _in_buffer.size()) {
            return ErrBufferLimitExceeded;
        }
        
        auto ret = _in.read(_in_buffer.allocate_back(bucket_read), bucket_read);
        if (ret >= 0) {
            _in_buffer.free_back(bucket_read - ret);
            const auto pending = SSL_pending(transport().ssl_handle());
            if (pending) {
                if (this->_max_read_buffer_size < _in_buffer.size() ||
                    static_cast<std::size_t>(pending) > this->_max_read_buffer_size - _in_buffer.size()) {
                    return ErrBufferLimitExceeded;
                }
                const auto pending_sz = static_cast<std::size_t>(pending);
                const auto ret2 = _in.read(_in_buffer.allocate_back(pending_sz), pending_sz);
                if (ret2 >= 0) {
                    _in_buffer.free_back(pending_sz - static_cast<std::size_t>(ret2));
                    ret += ret2;
                } else {
                    _in_buffer.free_back(pending_sz);
                }
            }
        } else {
            _in_buffer.free_back(bucket_read);
        }
        return ret;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_STCP_H
