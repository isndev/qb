/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_UNORDERED_MAP_H
#define QB_UNORDERED_MAP_H
#include <map>
#include <ska_hash/unordered_map.hpp>
#include <unordered_map>

namespace qb {

template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>,
          typename A = std::allocator<std::pair<const K, V>>>
using unordered_flat_map = ska::flat_hash_map<K, V, H, E, A>;
#ifdef NDEBUG
template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>,
          typename A = std::allocator<std::pair<const K, V>>>
using unordered_map = ska::unordered_map<K, V, H, E, A>;
#else
template <typename K, typename V, typename H = std::hash<K>,
          typename E = std::equal_to<K>,
          typename A = std::allocator<std::pair<const K, V>>>
using unordered_map = std::unordered_map<K, V, H, E, A>;
#endif

class string_to_lower {
    // Helper function that converts a character to lowercase on compile time
    constexpr static char
    charToLower(const char c) {
        return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
    }

    // Compile time string class that is used to pass around the converted string
    template <std::size_t N>
    class const_str {
    private:
        const char s[N + 1]; // One extra byte to fill with a 0 value

    public:
        // Constructor that is given the char array and an integer sequence to use
        // parameter pack expansion on the array
        template <typename T, T... Nums>
        constexpr const_str(const char (&str)[N], std::integer_sequence<T, Nums...>)
            : s{charToLower(str[Nums])..., 0} {}

        // Compile time access operator to the characters
        constexpr char
        operator[](std::size_t i) const {
            return s[i];
        }

        // Get a pointer to the array at runtime. Even though this happens at runtime,
        // this is a fast operation and much faster than the actual conversion
        operator const char *() const {
            return s;
        }
    };

    template <std::size_t N>
    constexpr static const_str<N>
    toLower(const char (&str)[N]) {
        return {str, std::make_integer_sequence<unsigned, N>()};
    }

public:
    template <std::size_t N>
    static std::string
    convert(const char (&str)[N]) {
        return std::string{toLower(str), N - 1};
    }

    static std::string
    convert(std::string const &str) {
        std::string copy;
        copy.reserve(str.size());
        std::for_each(str.cbegin(), str.cend(),
                      [&copy](const char &c) { copy += charToLower(c); });
        return copy;
    }

    static std::string
    convert(std::string_view const &str) {
        std::string copy;
        copy.reserve(str.size());
        std::for_each(str.cbegin(), str.cend(),
                      [&copy](const char &c) { copy += charToLower(c); });
        return copy;
    }
};

template <typename _Map, typename _Trait = string_to_lower>
class icase_basic_map : _Map {
    using base_t = _Map;

public:
    icase_basic_map() = default;
    icase_basic_map(icase_basic_map const &) = default;
    icase_basic_map(icase_basic_map &&) noexcept = default;
    icase_basic_map(std::initializer_list<typename _Map::value_type> il) {
        for (auto &it : il)
            emplace(it.first, std::move(it.second));
    }
    icase_basic_map &operator=(icase_basic_map const &) = default;
    icase_basic_map &operator=(icase_basic_map &&) noexcept = default;

    template <typename T, typename... _Args>
    auto
    emplace(T &&key, _Args &&... args) {
        return static_cast<base_t &>(*this).emplace(
            _Trait::convert(std::forward<T>(key)), std::forward<_Args>(args)...);
    }

    template <typename T, typename... _Args>
    auto
    try_emplace(T &&key, _Args &&... args) {
        return static_cast<base_t &>(*this).try_emplace(
            _Trait::convert(std::forward<T>(key)), std::forward<_Args>(args)...);
    }

    template <typename T>
    auto &
    at(T &&key) {
        return static_cast<base_t &>(*this).at(_Trait::convert(std::forward<T>(key)));
    }

    template <typename T>
    const auto &
    at(T &&key) const {
        return static_cast<base_t const &>(*this).at(
            _Trait::convert(std::forward<T>(key)));
    }

    template <typename T>
    auto &
    operator[](T &&key) {
        return static_cast<base_t &>(*this)[_Trait::convert(std::forward<T>(key))];
    }

    template <typename T>
    auto
    find(T &&key) {
        return static_cast<base_t &>(*this).find(_Trait::convert(std::forward<T>(key)));
    }

    template <typename T>
    auto
    find(T &&key) const {
        return static_cast<base_t const &>(*this).find(
            _Trait::convert(std::forward<T>(key)));
    }

    template <typename T>
    bool
    has(T &&key) const {
        return find(std::forward<T>(key)) != this->cend();
    }

    using base_t::begin;
    using base_t::cbegin;
    using base_t::cend;
    using base_t::clear;
    using base_t::end;
    using base_t::erase;
    using base_t::insert;
    using base_t::size;

    template <typename T>
    static std::string
    convert_key(T &&key) noexcept {
        return _Trait::convert(std::forward<T>(key));
    }
};

template <typename Value, typename _Trait = string_to_lower>
using icase_map = icase_basic_map<std::map<std::string, Value>, _Trait>;

template <typename Value, typename _Trait = string_to_lower>
using icase_unordered_map =
    icase_basic_map<qb::unordered_map<std::string, Value>, _Trait>;

} // namespace qb

#endif // QB_UNORDERED_MAP_H
