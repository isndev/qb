/**
 * @file qb/io/pipe.cpp
 * @brief Implementation of the pipe class specialized for characters
 *
 * This file contains template specializations for the pipe<char> class
 * which provide optimized implementations for character-based operations
 * like string handling, appending, and conversion.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <qb/system/allocator/pipe.h>

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<char>(const char &c) {
    *allocate_back(1) = c;
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<unsigned char>(const unsigned char &c) {
    *allocate_back(1) = static_cast<const char &>(c);
    return *this;
}

// Every overload below whose source may be a view of this very pipe reads it through
// allocate_back_from(): a growing or compacting allocate_back() moves the live bytes, and the
// copy must read them where they are AFTER the allocation (Huly QB-277). A std::string,
// qb::string, vector or array owns its storage and cannot alias the pipe.

template <>
pipe<char> &
pipe<char>::put<const char *>(const char *const &c) {
    const auto len = std::strlen(c);
    if (len) {
        const char *source = c;
        char *const space  = allocate_back_from(len, source);
        std::memcpy(space, source, len);
    }
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<std::string>(std::string const &str) {
    memcpy(allocate_back(str.size()), str.c_str(), str.size());
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<std::string_view>(std::string_view const &str) {
    if (const auto size = str.size()) {
        const char *source = str.data();
        char *const space  = allocate_back_from(size, source);
        std::memcpy(space, source, size);
    }
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<pipe<char>>(pipe<char> const &rhs) {
    // Size and source are read BEFORE the allocation: `rhs` may be *this (p.put(p)).
    if (const auto size = rhs.size()) {
        const char *source = rhs.begin();
        char *const space  = allocate_back_from(size, source);
        std::memcpy(space, source, size);
    }
    return *this;
}

pipe<char> &
pipe<char>::put(char const *data, std::size_t size) noexcept {
    // Guard size==0: a 0-length memcpy with a null `data` is UB (memcpy's source is
    // declared nonnull) — matches the put<string_view>/put<pipe> overloads above.
    if (size) {
        char *const space = allocate_back_from(size, data);
        memcpy(space, data, size);
    }
    return *this;
}

pipe<char> &
pipe<char>::write(const char *data, std::size_t size) noexcept {
    if (size) {
        char *const space = allocate_back_from(size, data);
        memcpy(space, data, size);
    }
    return *this;
}

std::string
pipe<char>::str() const noexcept {
    return std::string(cbegin(), size());
}

std::string_view
pipe<char>::view() const noexcept {
    return std::string_view(cbegin(), size());
}

std::ostream &
operator<<(std::ostream &os, pipe<char> const &p) {
    os << std::string_view(p.begin(), p.size());
    return os;
}

} // namespace qb::allocator