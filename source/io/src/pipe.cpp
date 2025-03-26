/**
 * @file qb/io/src/pipe.cpp
 * @brief Implementation of the pipe class specialized for characters
 * 
 * This file contains template specializations for the pipe<char> class
 * which provide optimized implementations for character-based operations
 * like string handling, appending, and conversion.
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

template <>
pipe<char> &
pipe<char>::put<const char *>(const char *const &c) {
    auto b = c;
    while (*b)
        put(*(b++));
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
    auto b = allocate_back(str.size());
    for (auto c : str) {
        *(b++) = c;
    }
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<pipe<char>>(pipe<char> const &rhs) {
    auto b = allocate_back(rhs.size());
    for (auto c : rhs) {
        *(b++) = c;
    }
    return *this;
}

pipe<char> &
pipe<char>::put(char const *data, std::size_t size) noexcept {
    memcpy(allocate_back(size), data, size);
    return *this;
}

pipe<char> &
pipe<char>::write(const char *data, std::size_t size) noexcept {
    memcpy(allocate_back(size), data, size);
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