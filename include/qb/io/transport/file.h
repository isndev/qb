/**
 * @file qb/io/transport/file.h
 * @brief File transport implementation for the QB IO library
 *
 * This file provides a transport implementation for file operations,
 * extending the stream class with file-specific functionality.
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

#ifndef QB_IO_TRANSPORT_FILE_H
#define QB_IO_TRANSPORT_FILE_H
#include "../stream.h"
#include "../system/file.h"

namespace qb::io::transport {

/**
 * @class file
 * @brief File transport class
 *
 * This class implements a transport layer for file operations by extending
 * the generic stream class with file-specific implementation. It provides
 * file I/O operations through the stream interface.
 */
class file : public stream<io::sys::file> {
public:
    /**
     * @brief Write data to the file
     * @return Always returns 0 as file writes are handled separately
     *
     * This method is a placeholder as file writing is handled through
     * other mechanisms in the file implementation.
     */
    [[nodiscard]] int
    write() {
        return 0;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_FILE_H
