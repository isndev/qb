/**
 * @file qb/io/async/file.h
 * @brief Asynchronous file operations for the QB IO library
 * 
 * This file defines the file class template which provides asynchronous 
 * file operations using the file_watcher mechanism. It allows for non-blocking
 * file I/O integrated with the event-based asynchronous framework.
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

#ifndef QB_IO_ASYNC_FILE_H
#define QB_IO_ASYNC_FILE_H

#include "../transport/file.h"
#include "io.h"

namespace qb::io::async {

/**
 * @class file
 * @brief Asynchronous file operations handler
 * 
 * This template class provides asynchronous file operations by combining
 * the file_watcher functionality with the file transport. It allows for
 * non-blocking file operations integrated with the event loop.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class file
    : public file_watcher<_Derived>
    , qb::io::transport::file {
    using base_t = file_watcher<_Derived>;
    friend base_t;
public:
    /**
     * @brief Type of the underlying transport IO
     */
    using transport_io_type = typename qb::io::transport::file::transport_io_type;
    
    using qb::io::transport::file::in;        /**< Import the in method from the file transport */
    using qb::io::transport::file::out;       /**< Import the out method from the file transport */
    using qb::io::transport::file::transport; /**< Import the transport method from the file transport */
    
public:
    /**
     * @brief Constructor
     * 
     * Creates a new asynchronous file handler. If the derived class
     * defines a Protocol type that is not void, an instance of that
     * protocol is created and attached to the file handler.
     */
    explicit file() {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                        static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Get a unique identifier for the file
     * 
     * Returns the native handle of the underlying file as a unique
     * identifier.
     * 
     * @return The file's native handle
     */
    inline uint64_t
    ident() noexcept {
        return static_cast<_Derived &>(*this).transport().native_handle();
    }
};

} // namespace qb::io::async::file

#endif // QB_IO_ASYNC_FILE_H
