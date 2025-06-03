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
 * @ingroup Container
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <qb/utility/build_macros.h>
#include <string>
#include <stdexcept>

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
    static_assert(_Size > 0, "String size must be greater than 0");
    
    using base_t    = std::array<char, _Size + 1>;
    using size_type = typename internal::best_size<_Size + 1>::type;
    
public:
    // Type aliases for compatibility with std::string
    using value_type             = char;
    using reference              = char&;
    using const_reference        = const char&;
    using pointer                = char*;
    using const_pointer          = const char*;
    using iterator               = typename base_t::iterator;
    using const_iterator         = typename base_t::const_iterator;
    using reverse_iterator       = typename base_t::reverse_iterator;
    using const_reverse_iterator = typename base_t::const_reverse_iterator;
    using difference_type        = typename base_t::difference_type;
    
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

private:
    size_type _size = 0;

public:
    /**
     * @brief Default constructor
     *
     * Creates an empty string with a null terminator
     */
    constexpr string() noexcept
        : base_t{'\0'} {}

    /**
     * @brief Constructor from C-style string literal
     *
     * @tparam N Size of the string literal including null terminator
     * @param str The string literal to copy
     */
    template <std::size_t N>
    constexpr string(const char (&str)[N]) noexcept {
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
     * @brief Constructor from C-style string and size
     *
     * @param str C-style string to copy from
     * @param size Number of characters to copy
     */
    string(const char* str, std::size_t size) noexcept {
        assign(str, size);
    }

    /**
     * @brief Constructor from C-style string
     *
     * @param str Null-terminated C-style string
     */
    string(const char* str) noexcept {
        assign(str);
    }

    /**
     * @brief Fill constructor
     *
     * @param count Number of characters to fill
     * @param ch Character to fill with
     */
    string(std::size_t count, char ch) noexcept {
        _size = static_cast<size_type>(std::min(count, static_cast<std::size_t>(_Size)));
        std::fill_n(base_t::data(), _size, ch);
        base_t::data()[_size] = '\0';
    }

    /**
     * @brief Copy constructor
     */
    string(const string& other) noexcept = default;

    /**
     * @brief Move constructor
     */
    string(string&& other) noexcept = default;

    /**
     * @brief Copy assignment operator
     */
    string& operator=(const string& other) noexcept = default;

    /**
     * @brief Move assignment operator
     */
    string& operator=(string&& other) noexcept = default;

    /**
     * @brief Assign from C-style string and size
     *
     * @param rhs C-style string to copy from
     * @param size Number of characters to copy
     * @return Reference to this string
     */
    string &
    assign(char const *rhs, std::size_t size) noexcept {
        _size = static_cast<size_type>(std::min(size, static_cast<std::size_t>(_Size)));

        std::memcpy(base_t::data(), rhs, _size);
        base_t::data()[_size] = '\0';

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
    constexpr string &
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
        return assign(rhs.data(), rhs.size());
    }

    /**
     * @brief Assign from C-style string
     *
     * @param rhs Null-terminated C-style string
     * @return Reference to this string
     */
    string &
    assign(char const *rhs) noexcept {
        return assign(rhs, std::strlen(rhs));
    }

    /**
     * @brief Assign with fill
     *
     * @param count Number of characters to assign
     * @param ch Character to fill with
     * @return Reference to this string
     */
    string &
    assign(std::size_t count, char ch) noexcept {
        _size = static_cast<size_type>(std::min(count, static_cast<std::size_t>(_Size)));
        std::fill_n(base_t::data(), _size, ch);
        base_t::data()[_size] = '\0';
        return *this;
    }

    /**
     * @brief Assignment operator for C-style string literals
     *
     * @tparam N Size of the string literal including null terminator
     * @param str The string literal to assign
     * @return Reference to this string
     */
    template <std::size_t N>
    constexpr string &
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
     * @brief Assignment operator for single character
     *
     * @param ch Character to assign
     * @return Reference to this string
     */
    string &
    operator=(char ch) noexcept {
        return assign(1, ch);
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

    // Element access
    /**
     * @brief Access character at index with bounds checking
     *
     * @param pos Index of the character
     * @return Reference to the character
     * @throws std::out_of_range if pos >= size()
     */
    reference at(std::size_t pos) {
        if (pos >= _size) {
            throw std::out_of_range("qb::string::at: index out of range");
        }
        return base_t::data()[pos];
    }

    /**
     * @brief Access character at index with bounds checking (const version)
     *
     * @param pos Index of the character
     * @return Const reference to the character
     * @throws std::out_of_range if pos >= size()
     */
    const_reference at(std::size_t pos) const {
        if (pos >= _size) {
            throw std::out_of_range("qb::string::at: index out of range");
        }
        return base_t::data()[pos];
    }

    /**
     * @brief Access character at index (no bounds checking)
     *
     * @param pos Index of the character
     * @return Reference to the character
     */
    reference operator[](std::size_t pos) noexcept {
        return base_t::data()[pos];
    }

    /**
     * @brief Access character at index (no bounds checking, const version)
     *
     * @param pos Index of the character
     * @return Const reference to the character
     */
    const_reference operator[](std::size_t pos) const noexcept {
        return base_t::data()[pos];
    }

    /**
     * @brief Access first character
     *
     * @return Reference to the first character
     */
    reference front() noexcept {
        return base_t::data()[0];
    }

    /**
     * @brief Access first character (const version)
     *
     * @return Const reference to the first character
     */
    const_reference front() const noexcept {
        return base_t::data()[0];
    }

    /**
     * @brief Access last character
     *
     * @return Reference to the last character
     */
    reference back() noexcept {
        return base_t::data()[_size - 1];
    }

    /**
     * @brief Access last character (const version)
     *
     * @return Const reference to the last character
     */
    const_reference back() const noexcept {
        return base_t::data()[_size - 1];
    }

    /**
     * @brief Get pointer to underlying data
     *
     * @return Pointer to the underlying character array
     */
    pointer data() noexcept {
        return base_t::data();
    }

    /**
     * @brief Get pointer to underlying data (const version)
     *
     * @return Const pointer to the underlying character array
     */
    const_pointer data() const noexcept {
        return base_t::data();
    }

    /**
     * @brief Get C-style string representation
     *
     * @return Null-terminated C-style string
     */
    [[nodiscard]] const_pointer c_str() const noexcept {
        return base_t::data();
    }

    // Iterators
    /**
     * @brief Get iterator to the end of the string
     *
     * @return Iterator pointing just past the last character
     */
    iterator end() noexcept {
        return base_t::begin() + _size;
    }

    /**
     * @brief Get const iterator to the end of the string
     *
     * @return Const iterator pointing just past the last character
     */
    const_iterator end() const noexcept {
        return base_t::begin() + _size;
    }

    /**
     * @brief Get const iterator to the end of the string
     *
     * @return Const iterator pointing just past the last character
     */
    const_iterator cend() const noexcept {
        return base_t::cbegin() + _size;
    }

    /**
     * @brief Get reverse iterator to the beginning of the string
     *
     * @return Reverse iterator pointing to the last character
     */
    reverse_iterator rbegin() noexcept {
        return std::reverse_iterator(end());
    }

    /**
     * @brief Get const reverse iterator to the beginning of the string
     *
     * @return Const reverse iterator pointing to the last character
     */
    const_reverse_iterator rbegin() const noexcept {
        return std::reverse_iterator(end());
    }

    /**
     * @brief Get const reverse iterator to the beginning of the string
     *
     * @return Const reverse iterator pointing to the last character
     */
    const_reverse_iterator crbegin() const noexcept {
        return std::reverse_iterator(cend());
    }

    // Capacity
    /**
     * @brief Check if the string is empty
     *
     * @return true if the string is empty, false otherwise
     */
    [[nodiscard]] constexpr bool empty() const noexcept {
        return _size == 0;
    }

    /**
     * @brief Get the string length
     *
     * @return Number of characters in the string
     */
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return _size;
    }

    /**
     * @brief Get the string length (alias for size())
     *
     * @return Number of characters in the string
     */
    [[nodiscard]] constexpr std::size_t length() const noexcept {
        return _size;
    }

    /**
     * @brief Get the maximum possible size
     *
     * @return Maximum number of characters the string can hold
     */
    [[nodiscard]] constexpr std::size_t max_size() const noexcept {
        return _Size;
    }

    /**
     * @brief Get the capacity
     *
     * @return Maximum number of characters the string can hold
     */
    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return _Size;
    }

    // Operations
    /**
     * @brief Clear the string
     *
     * Sets the string to an empty state with a null terminator
     */
    void clear() noexcept {
        *base_t::begin() = '\0';
        _size = 0;
    }

    /**
     * @brief Resize the string
     *
     * @param count New size of the string
     * @param ch Character to fill with if expanding
     */
    void resize(std::size_t count, char ch = '\0') noexcept {
        count = std::min(count, static_cast<std::size_t>(_Size));
        
        if (count > _size) {
            std::fill(base_t::data() + _size, base_t::data() + count, ch);
        }
        
        _size = static_cast<size_type>(count);
        base_t::data()[_size] = '\0';
    }

    /**
     * @brief Swap contents with another string
     *
     * @param other String to swap with
     */
    void swap(string& other) noexcept {
        base_t temp_data = *this;
        size_type temp_size = _size;
        
        *this = other;
        _size = other._size;
        
        other = temp_data;
        other._size = temp_size;
    }

    // String operations
    /**
     * @brief Extract substring
     *
     * @param pos Starting position
     * @param len Number of characters to extract
     * @return New string containing the substring
     */
    string substr(std::size_t pos = 0, std::size_t len = npos) const {
        if (pos > _size) {
            throw std::out_of_range("qb::string::substr: position out of range");
        }
        
        std::size_t actual_len = std::min(len, _size - pos);
        string result;
        result.assign(base_t::data() + pos, actual_len);
        return result;
    }

    /**
     * @brief Compare with another string
     *
     * @param str String to compare with
     * @return Negative if this < str, 0 if equal, positive if this > str
     */
    int compare(const string& str) const noexcept {
        return std::strcmp(base_t::data(), str.c_str());
    }

    /**
     * @brief Compare with C-style string
     *
     * @param str C-style string to compare with
     * @return Negative if this < str, 0 if equal, positive if this > str
     */
    int compare(const char* str) const noexcept {
        return std::strcmp(base_t::data(), str);
    }

    /**
     * @brief Compare substring with another string
     *
     * @param pos Starting position in this string
     * @param len Number of characters to compare
     * @param str String to compare with
     * @return Negative if this < str, 0 if equal, positive if this > str
     */
    int compare(std::size_t pos, std::size_t len, const string& str) const {
        return substr(pos, len).compare(str);
    }

    // Search operations
    /**
     * @brief Find substring
     *
     * @param str Substring to find
     * @param pos Starting position
     * @return Position of first occurrence or npos if not found
     */
    std::size_t find(const string& str, std::size_t pos = 0) const noexcept {
        return find(str.c_str(), pos);
    }

    /**
     * @brief Find C-style string
     *
     * @param str C-style string to find
     * @param pos Starting position
     * @return Position of first occurrence or npos if not found
     */
    std::size_t find(const char* str, std::size_t pos = 0) const noexcept {
        if (pos >= _size) return npos;
        
        const char* result = std::strstr(base_t::data() + pos, str);
        return result ? static_cast<std::size_t>(result - base_t::data()) : npos;
    }

    /**
     * @brief Find character
     *
     * @param ch Character to find
     * @param pos Starting position
     * @return Position of first occurrence or npos if not found
     */
    std::size_t find(char ch, std::size_t pos = 0) const noexcept {
        if (pos >= _size) return npos;
        
        const char* result = std::strchr(base_t::data() + pos, ch);
        return (result && result < base_t::data() + _size) ? 
               static_cast<std::size_t>(result - base_t::data()) : npos;
    }

    /**
     * @brief Find last occurrence of substring
     *
     * @param str Substring to find
     * @param pos Starting position (searches backwards from here)
     * @return Position of last occurrence or npos if not found
     */
    std::size_t rfind(const char* str, std::size_t pos = npos) const noexcept {
        std::size_t str_len = std::strlen(str);
        if (str_len > _size) return npos;
        
        std::size_t start = std::min(pos, _size - str_len);
        
        for (std::size_t i = start + 1; i > 0; --i) {
            if (std::strncmp(base_t::data() + i - 1, str, str_len) == 0) {
                return i - 1;
            }
        }
        return npos;
    }

    /**
     * @brief Find last occurrence of character
     *
     * @param ch Character to find
     * @param pos Starting position (searches backwards from here)
     * @return Position of last occurrence or npos if not found
     */
    std::size_t rfind(char ch, std::size_t pos = npos) const noexcept {
        if (_size == 0) return npos;
        
        std::size_t start = std::min(pos, static_cast<std::size_t>(_size - 1));
        
        for (std::size_t i = start + 1; i > 0; --i) {
            if (base_t::data()[i - 1] == ch) {
                return i - 1;
            }
        }
        return npos;
    }

    // Modifiers
    /**
     * @brief Append string
     *
     * @param str String to append
     * @return Reference to this string
     */
    string& append(const string& str) noexcept {
        return append(str.c_str(), str.size());
    }

    /**
     * @brief Append C-style string
     *
     * @param str C-style string to append
     * @return Reference to this string
     */
    string& append(const char* str) noexcept {
        return append(str, std::strlen(str));
    }

    /**
     * @brief Append C-style string with length
     *
     * @param str C-style string to append
     * @param len Number of characters to append
     * @return Reference to this string
     */
    string& append(const char* str, std::size_t len) noexcept {
        std::size_t new_size = std::min(_size + len, static_cast<std::size_t>(_Size));
        std::size_t actual_len = new_size - _size;
        
        std::memcpy(base_t::data() + _size, str, actual_len);
        _size = static_cast<size_type>(new_size);
        base_t::data()[_size] = '\0';
        
        return *this;
    }

    /**
     * @brief Append character
     *
     * @param ch Character to append
     * @return Reference to this string
     */
    string& append(char ch) noexcept {
        return append(1, ch);
    }

    /**
     * @brief Append multiple characters
     *
     * @param count Number of characters to append
     * @param ch Character to append
     * @return Reference to this string
     */
    string& append(std::size_t count, char ch) noexcept {
        std::size_t new_size = std::min(_size + count, static_cast<std::size_t>(_Size));
        std::size_t actual_count = new_size - _size;
        
        std::fill_n(base_t::data() + _size, actual_count, ch);
        _size = static_cast<size_type>(new_size);
        base_t::data()[_size] = '\0';
        
        return *this;
    }

    /**
     * @brief Add character to end
     *
     * @param ch Character to add
     */
    void push_back(char ch) noexcept {
        if (_size < _Size) {
            base_t::data()[_size] = ch;
            ++_size;
            base_t::data()[_size] = '\0';
        }
    }

    /**
     * @brief Remove last character
     */
    void pop_back() noexcept {
        if (_size > 0) {
            --_size;
            base_t::data()[_size] = '\0';
        }
    }

    /**
     * @brief Append operator
     *
     * @param str String to append
     * @return Reference to this string
     */
    string& operator+=(const string& str) noexcept {
        return append(str);
    }

    /**
     * @brief Append operator for C-style string
     *
     * @param str C-style string to append
     * @return Reference to this string
     */
    string& operator+=(const char* str) noexcept {
        return append(str);
    }

    /**
     * @brief Append operator for character
     *
     * @param ch Character to append
     * @return Reference to this string
     */
    string& operator+=(char ch) noexcept {
        push_back(ch);
        return *this;
    }

    // C++20 features
    /**
     * @brief Check if string starts with given prefix
     *
     * @param prefix Prefix to check
     * @return true if string starts with prefix, false otherwise
     */
    bool starts_with(const char* prefix) const noexcept {
        std::size_t prefix_len = std::strlen(prefix);
        return _size >= prefix_len && std::strncmp(base_t::data(), prefix, prefix_len) == 0;
    }

    /**
     * @brief Check if string starts with given prefix
     *
     * @param prefix Prefix to check
     * @return true if string starts with prefix, false otherwise
     */
    bool starts_with(const string& prefix) const noexcept {
        return starts_with(prefix.c_str());
    }

    /**
     * @brief Check if string starts with given character
     *
     * @param ch Character to check
     * @return true if string starts with character, false otherwise
     */
    bool starts_with(char ch) const noexcept {
        return _size > 0 && base_t::data()[0] == ch;
    }

    /**
     * @brief Check if string ends with given suffix
     *
     * @param suffix Suffix to check
     * @return true if string ends with suffix, false otherwise
     */
    bool ends_with(const char* suffix) const noexcept {
        std::size_t suffix_len = std::strlen(suffix);
        return _size >= suffix_len && 
               std::strncmp(base_t::data() + _size - suffix_len, suffix, suffix_len) == 0;
    }

    /**
     * @brief Check if string ends with given suffix
     *
     * @param suffix Suffix to check
     * @return true if string ends with suffix, false otherwise
     */
    bool ends_with(const string& suffix) const noexcept {
        return ends_with(suffix.c_str());
    }

    /**
     * @brief Check if string ends with given character
     *
     * @param ch Character to check
     * @return true if string ends with character, false otherwise
     */
    bool ends_with(char ch) const noexcept {
        return _size > 0 && base_t::data()[_size - 1] == ch;
    }

    /**
     * @brief Check if string contains given substring
     *
     * @param str Substring to find
     * @return true if string contains substring, false otherwise
     */
    bool contains(const char* str) const noexcept {
        return find(str) != npos;
    }

    /**
     * @brief Check if string contains given substring
     *
     * @param str Substring to find
     * @return true if string contains substring, false otherwise
     */
    bool contains(const string& str) const noexcept {
        return find(str) != npos;
    }

    /**
     * @brief Check if string contains given character
     *
     * @param ch Character to find
     * @return true if string contains character, false otherwise
     */
    bool contains(char ch) const noexcept {
        return find(ch) != npos;
    }

    // Comparison operators
    /**
     * @brief Equality comparison with string-like object
     *
     * @tparam T Type with operator== for C-style strings
     * @param rhs String-like object to compare with
     * @return true if strings are equal, false otherwise
     */
    template <typename T>
    bool operator==(T const &rhs) const noexcept {
        return rhs == base_t::data();
    }

    /**
     * @brief Equality comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if strings are equal, false otherwise
     */
    bool operator==(char const *rhs) const noexcept {
        return std::strcmp(base_t::data(), rhs) == 0;
    }

    /**
     * @brief Equality comparison with another qb::string
     *
     * @param rhs String to compare with
     * @return true if strings are equal, false otherwise
     */
    bool operator==(const string& rhs) const noexcept {
        return _size == rhs._size && std::strcmp(base_t::data(), rhs.c_str()) == 0;
    }

    /**
     * @brief Inequality comparison
     *
     * @param rhs String to compare with
     * @return true if strings are not equal, false otherwise
     */
    bool operator!=(const string& rhs) const noexcept {
        return !(*this == rhs);
    }

    /**
     * @brief Inequality comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if strings are not equal, false otherwise
     */
    bool operator!=(const char* rhs) const noexcept {
        return !(*this == rhs);
    }

    /**
     * @brief Less than comparison
     *
     * @param rhs String to compare with
     * @return true if this string is lexicographically less than rhs
     */
    bool operator<(const string& rhs) const noexcept {
        return compare(rhs) < 0;
    }

    /**
     * @brief Less than comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if this string is lexicographically less than rhs
     */
    bool operator<(const char* rhs) const noexcept {
        return compare(rhs) < 0;
    }

    /**
     * @brief Greater than comparison
     *
     * @param rhs String to compare with
     * @return true if this string is lexicographically greater than rhs
     */
    bool operator>(const string& rhs) const noexcept {
        return compare(rhs) > 0;
    }

    /**
     * @brief Greater than comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if this string is lexicographically greater than rhs
     */
    bool operator>(const char* rhs) const noexcept {
        return compare(rhs) > 0;
    }

    /**
     * @brief Less than or equal comparison
     *
     * @param rhs String to compare with
     * @return true if this string is lexicographically less than or equal to rhs
     */
    bool operator<=(const string& rhs) const noexcept {
        return compare(rhs) <= 0;
    }

    /**
     * @brief Less than or equal comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if this string is lexicographically less than or equal to rhs
     */
    bool operator<=(const char* rhs) const noexcept {
        return compare(rhs) <= 0;
    }

    /**
     * @brief Greater than or equal comparison
     *
     * @param rhs String to compare with
     * @return true if this string is lexicographically greater than or equal to rhs
     */
    bool operator>=(const string& rhs) const noexcept {
        return compare(rhs) >= 0;
    }

    /**
     * @brief Greater than or equal comparison with C-style string
     *
     * @param rhs C-style string to compare with
     * @return true if this string is lexicographically greater than or equal to rhs
     */
    bool operator>=(const char* rhs) const noexcept {
        return compare(rhs) >= 0;
    }
};

// Non-member functions

/**
 * @brief Concatenation operator
 *
 * @tparam _Size1 Size of the first string
 * @tparam _Size2 Size of the second string
 * @param lhs First string
 * @param rhs Second string
 * @return New string containing concatenated result
 */
template <std::size_t _Size1, std::size_t _Size2>
string<std::max(_Size1, _Size2)> operator+(const string<_Size1>& lhs, const string<_Size2>& rhs) noexcept {
    string<std::max(_Size1, _Size2)> result(lhs);
    result.append(rhs);
    return result;
}

/**
 * @brief Concatenation operator with C-style string
 *
 * @tparam _Size Size of the string
 * @param lhs String
 * @param rhs C-style string
 * @return New string containing concatenated result
 */
template <std::size_t _Size>
string<_Size> operator+(const string<_Size>& lhs, const char* rhs) noexcept {
    string<_Size> result(lhs);
    result.append(rhs);
    return result;
}

/**
 * @brief Concatenation operator with C-style string on left
 *
 * @tparam _Size Size of the string
 * @param lhs C-style string
 * @param rhs String
 * @return New string containing concatenated result
 */
template <std::size_t _Size>
string<_Size> operator+(const char* lhs, const string<_Size>& rhs) noexcept {
    string<_Size> result(lhs);
    result.append(rhs);
    return result;
}

/**
 * @brief Concatenation operator with character
 *
 * @tparam _Size Size of the string
 * @param lhs String
 * @param rhs Character
 * @return New string containing concatenated result
 */
template <std::size_t _Size>
string<_Size> operator+(const string<_Size>& lhs, char rhs) noexcept {
    string<_Size> result(lhs);
    result.push_back(rhs);
    return result;
}

/**
 * @brief Concatenation operator with character on left
 *
 * @tparam _Size Size of the string
 * @param lhs Character
 * @param rhs String
 * @return New string containing concatenated result
 */
template <std::size_t _Size>
string<_Size> operator+(char lhs, const string<_Size>& rhs) noexcept {
    string<_Size> result(1, lhs);
    result.append(rhs);
    return result;
}

/**
 * @brief Swap function for strings
 *
 * @tparam _Size Size of the strings
 * @param lhs First string
 * @param rhs Second string
 */
template <std::size_t _Size>
void swap(string<_Size>& lhs, string<_Size>& rhs) noexcept {
    lhs.swap(rhs);
}

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
operator>>(std::istream &is, qb::string<_Size> &str) noexcept {
    std::string temp;
    is >> temp;
    str = temp;
    return is;
}

// Equality operators for reverse comparisons
template <std::size_t _Size>
bool operator==(const char* lhs, const string<_Size>& rhs) noexcept {
    return rhs == lhs;
}

template <std::size_t _Size>
bool operator!=(const char* lhs, const string<_Size>& rhs) noexcept {
    return rhs != lhs;
}

} // namespace qb

#endif // QB_STRING_H_
