/**
 * @file qb/string.h
 * @brief Fixed-size string implementation optimized for performance
 *
 * This file defines a string class with a fixed maximum size that provides
 * better performance than std::string for small strings that don't exceed
 * the size limit. It uses a template parameter to specify the maximum
 * string size and chooses the optimal size type for storage.
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
 * @ingroup Utility
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <qb/utility/build_macros.h>
#include <string>

#ifndef QB_STRING_H_
#define QB_STRING_H_

namespace qb {

namespace internal {
/**
 * @struct best_size
 * @brief Determines the optimal integer type for size storage based on max capacity
 *
 * Selects the smallest possible unsigned integer type that can hold the
 * size value, which helps minimize memory usage.
 *
 * @tparam _Size Maximum size of the string + 1 (for null terminator)
 * @tparam fill8 True if size fits in uint8_t
 * @tparam fill16 True if size fits in uint16_t
 */
template <std::size_t _Size, bool fill8 = (_Size <= std::numeric_limits<uint8_t>::max()),
          bool fill16 = (_Size <= std::numeric_limits<uint16_t>::max())>
struct best_size {
    using type = std::size_t;
};

/**
 * @brief Specialization for sizes that fit in uint8_t
 */
template <std::size_t _Size>
struct best_size<_Size, true, true> {
    using type = uint8_t;
};

/**
 * @brief Specialization for sizes that fit in uint16_t but not uint8_t
 */
template <std::size_t _Size>
struct best_size<_Size, false, true> {
    using type = uint16_t;
};
} // namespace internal

/**
 * @class string
 * @brief Fixed-size string with optimized storage
 *
 * A string class with a fixed maximum capacity that is more efficient
 * than std::string for small strings. It uses a std::array for storage
 * and provides common string operations with bounds checking.
 *
 * @tparam _Size Maximum size of the string (default: 30)
 */
template <std::size_t _Size = 30>
class string : public std::array<char, _Size + 1> {
    using base_t    = std::array<char, _Size + 1>;
    using size_type = typename internal::best_size<_Size + 1>::type;
    size_type _size = 0;

public:
    /**
     * @brief Default constructor
     *
     * Creates an empty string with a null terminator
     */
    string() noexcept
        : base_t{'\0'} {}

    /**
     * @brief Constructor from C-style string literal
     *
     * @tparam N Size of the string literal including null terminator
     * @param str The string literal to copy
     */
    template <std::size_t N>
    string(const char (&str)[N]) noexcept {
        assign(str, N - 1);
    }

    /**
     * @brief Constructor from string-like object
     *
     * @tparam T Type with c_str() and size() methods (e.g., std::string)
     * @param rhs String-like object to copy
     */
    template <typename T>
    string(T const &rhs) noexcept {
        assign(rhs);
    }

    /**
     * @brief Assign from C-style string and size
     *
     * @param rhs C-style string to copy from
     * @param size Number of characters to copy
     * @return Reference to this string
     */
    string &
    assign(char const *rhs, std::size_t size) noexcept {
        *base_t::begin() = '\0';
        _size = static_cast<size_type>(std::min(size, static_cast<std::size_t>(_Size)));
        strncat(base_t::data(), rhs, _size);
        return *this;
    }

    /**
     * @brief Assign from C-style string literal
     *
     * @tparam N Size of the string literal including null terminator
     * @param str The string literal to copy
     * @return Reference to this string
     */
    template <std::size_t N>
    string &
    assign(const char (&str)[N]) noexcept {
        return assign(str, N - 1);
    }

    /**
     * @brief Assign from string-like object
     *
     * @tparam T Type with c_str() and size() methods
     * @param rhs String-like object to copy
     * @return Reference to this string
     */
    template <typename T>
    string &
    assign(T const &rhs) noexcept {
        return assign(rhs.c_str(), rhs.size());
    }

    /**
     * @brief Assign from C-style string
     *
     * @param rhs Null-terminated C-style string
     * @return Reference to this string
     */
    string &
    assign(char const *rhs) noexcept {
        return assign(rhs, strlen(rhs));
    }

    /**
     * @brief Assignment operator for C-style string literals
     *
     * @tparam N Size of the string literal including null terminator
     * @param str The string literal to assign
     * @return Reference to this string
     */
    template <std::size_t N>
    string &
    operator=(const char (&str)[N]) noexcept {
        return assign(str, N - 1);
    }

    /**
     * @brief Assignment operator for string-like objects
     *
     * @tparam T Type with c_str() and size() methods
     * @param rhs String-like object to assign
     * @return Reference to this string
     */
    template <typename T>
    string &
    operator=(T const &rhs) noexcept {
        return assign(rhs);
    }

    /**
     * @brief Assignment operator for C-style strings
     *
     * @param rhs Null-terminated C-style string to assign
     * @return Reference to this string
     */
    string &
    operator=(char const *rhs) noexcept {
        return assign(rhs);
    }

    /**
     * @brief Conversion operator to std::string
     *
     * @return A std::string copy of this string
     */
    operator std::string() const noexcept {
        return {base_t::data(), _size};
    }

    /**
     * @brief Conversion operator to std::string_view
     *
     * @return A std::string_view referring to this string
     */
    operator std::string_view() const noexcept {
        return {base_t::data(), _size};
    }

    /**
     * @brief Equality comparison with string-like object
     *
     * @tparam T Type with operator== for C-style strings
     * @param rhs String-like object to compare with
     * @return true if strings are equal, false otherwise
     */
    template <typename T>
    bool
    operator==(T const &rhs) const noexcept {
        return rhs == base_t::data();
    }

    /**
     * @brief Equality comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if strings are equal, false otherwise
     */
    bool
    operator==(char const *rhs) const noexcept {
        return !strcmp(base_t::data(), rhs);
    }

    /**
     * @brief Get iterator to the end of the string
     *
     * @return Iterator pointing just past the last character
     */
    typename base_t::iterator
    end() noexcept {
        return base_t::begin() + _size;
    }

    /**
     * @brief Get const iterator to the end of the string
     *
     * @return Const iterator pointing just past the last character
     */
    typename base_t::const_iterator
    cend() const noexcept {
        return base_t::cbegin() + _size;
    }

    /**
     * @brief Get reverse iterator to the beginning of the string
     *
     * @return Reverse iterator pointing to the last character
     */
    typename base_t::reverse_iterator
    rbegin() noexcept {
        return std::reverse_iterator(end());
    }

    /**
     * @brief Get const reverse iterator to the beginning of the string
     *
     * @return Const reverse iterator pointing to the last character
     */
    typename base_t::const_reverse_iterator
    crbegin() const noexcept {
        return std::reverse_iterator(cend());
    }

    /**
     * @brief Get C-style string representation
     *
     * @return Null-terminated C-style string
     */
    [[nodiscard]] char const *
    c_str() const noexcept {
        return base_t::data();
    }

    /**
     * @brief Get the string length
     *
     * @return Number of characters in the string
     */
    [[nodiscard]] std::size_t
    size() const noexcept {
        return _size;
    }
};

/**
 * @brief Output stream operator for qb::string
 *
 * @tparam _Size Maximum size of the string
 * @param os Output stream
 * @param str String to output
 * @return Reference to the output stream
 */
template <std::size_t _Size>
std::ostream &
operator<<(std::ostream &os, qb::string<_Size> const &str) noexcept {
    os << str.c_str();
    return os;
}

/**
 * @brief Input stream operator for qb::string
 *
 * @tparam _Size Maximum size of the string
 * @param is Input stream
 * @param str String to read into
 * @return Reference to the input stream
 */
template <std::size_t _Size>
std::istream &
operator>>(std::istream &is, qb::string<_Size> const &str) noexcept {
    str.c_str() >> is;
    return is;
}

} // namespace qb

#endif // QB_STRING_H_
