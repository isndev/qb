/**
 * @file qb/system/container/unordered_map.h
 * @brief Optimized unordered map implementations
 *
 * This file provides optimized and specialized unordered map implementations
 * for the QB framework. It includes high-performance alternatives to the
 * standard unordered_map using flat hash maps from the ska_hash library,
 * and case-insensitive string map implementations.
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
 * @ingroup Container
 */

#ifndef QB_UNORDERED_MAP_H
#define QB_UNORDERED_MAP_H
#include <algorithm>
#include <map>
#include <qb/vendor/ska_hash/unordered_map.hpp>
#include <string>
#include <unordered_map>

namespace qb {

/**
 * @brief A high-performance flat hash map implementation
 *
 * This is a type alias for ska::flat_hash_map which provides better performance
 * characteristics than std::unordered_map for many use cases. It uses open addressing
 * with robin hood hashing for better cache locality and performance.
 *
 * @tparam K The key type
 * @tparam V The value type
 * @tparam H The hash function type (defaults to std::hash<K>)
 * @tparam E The equality function type (defaults to std::equal_to<K>)
 * @tparam A The allocator type
 */
template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>, typename A = std::allocator<std::pair<const K, V>>>
using unordered_flat_map = ska::flat_hash_map<K, V, H, E, A>;

/**
 * @brief The primary unordered map implementation
 *
 * A node-based hash map with a chained bucket array: the value lives in an
 * individually allocated `sherwood_v10_entry`, so references and pointers to
 * elements survive a rehash (only iterators are invalidated) — the same
 * stability contract `std::unordered_map` offers, with better locality on the
 * bucket walk. qb relies on that: `VirtualCore::ActorMap`, `Main::_registered_services`
 * and `router::memh::_registered_events` are all held across insertions.
 *
 * @warning **This alias is unconditional, and must stay that way.** Until 3.0.0 it
 *          resolved to `ska::unordered_map` under `NDEBUG` and to `std::unordered_map`
 *          otherwise (added 2020-03-30 in 5c94d026, with no recorded rationale beyond
 *          "better debugging support" — i.e. debugger pretty-printers). That made the
 *          *identity and layout* of a public type depend on a build macro:
 *          `sizeof(qb::unordered_map<int,int>)` was 32 with `NDEBUG` and 40 without,
 *          and the type is a data member of public classes (`qb::VirtualCore`,
 *          `qb::Main`, `qb::router::*`) and of qbm's public headers. A consumer
 *          compiled without `NDEBUG` — `CMAKE_BUILD_TYPE=Debug`, or simply *unset*,
 *          which is the default — against a Release-built libqb therefore read a
 *          `ska` map through `std` layout and aborted at runtime
 *          (`std::overflow_error: __next_prime overflow`).
 *
 *          A public type must not change identity with a build macro. Debug-time
 *          diagnostics are a separate, opt-in concern and must not alter the public
 *          type; nothing in qb consumed the `std::` branch other than the debugger.
 *          `qbmModuleConfig.cmake.in` carries the matching configure-time tripwire
 *          (`QB_ABI_UNORDERED_MAP`) so a future reintroduction fails loudly at
 *          `find_package()` time instead of at runtime.
 *
 * @note **No `contains()` — use `find(k) != end()`.** This is `ska::unordered_map`, a
 *       vendored third-party container (`qb/vendor/ska_hash/unordered_map.hpp`) written
 *       before C++20 added `contains()`, and it does not provide one. It is *not* a
 *       `std::unordered_map`, so the drop-in assumption fails here and only here — the rest
 *       of the interface (`find`, `count`, `at`, `operator[]`, `emplace`, iterators) matches.
 *       Adding the member would mean editing the vendored file, which the vendor-attribution
 *       contract forbids: that header is carried verbatim so its upstream license and
 *       provenance stay checkable, and `dev/agent/verify.sh`'s vendor-attribution leg
 *       enforces it. `count(k) != 0` is equivalent and equally O(1) for a unique-key map.
 *
 * @tparam K The key type
 * @tparam V The value type
 * @tparam H The hash function type (defaults to std::hash<K>)
 * @tparam E The equality function type (defaults to std::equal_to<K>)
 * @tparam A The allocator type
 * @ingroup Container
 */
template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>, typename A = std::allocator<std::pair<const K, V>>>
using unordered_map = ska::unordered_map<K, V, H, E, A>;

/**
 * @brief Utility class for case-insensitive string operations
 *
 * Provides functions to convert strings to lowercase for case-insensitive
 * comparisons and lookups, with both compile-time and runtime implementations.
 */
class string_to_lower {
    /**
     * @brief Helper function that converts a character to lowercase at compile time
     *
     * @param c The character to convert
     * @return The lowercase version of the character
     */
    constexpr static char
    charToLower(const char c) {
        return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
    }

    /**
     * @brief Compile-time string class for storing converted lowercase strings
     *
     * @tparam N Size of the string
     */
    template <std::size_t N>
    class const_str {
    private:
        const char s[N + 1]; // One extra byte to fill with a 0 value

    public:
        /**
         * @brief Constructor that converts a string to lowercase at compile-time
         *
         * @tparam T Integer sequence type
         * @tparam Nums Parameter pack of indices
         * @param str The input string
         * @param unused Integer sequence for parameter pack expansion
         */
        template <typename T, T... Nums>
        constexpr const_str(const char (&str)[N], std::integer_sequence<T, Nums...>)
            : s{charToLower(str[Nums])..., 0} {}

        /**
         * @brief Compile-time access operator to access characters in the string
         *
         * @param i Index of the character to access
         * @return The character at the given index
         */
        constexpr char
        operator[](std::size_t i) const {
            return s[i];
        }

        /**
         * @brief Conversion operator to const char* for runtime access
         *
         * @return Pointer to the internal string array
         */
        operator const char *() const {
            return s;
        }
    };

    /**
     * @brief Creates a lowercase version of a string at compile-time
     *
     * @tparam N Size of the string
     * @param str The input string
     * @return Lowercase version of the input string
     */
    template <std::size_t N>
    constexpr static const_str<N>
    toLower(const char (&str)[N]) {
        return {str, std::make_integer_sequence<unsigned, N>()};
    }

public:
    /**
     * @brief Converts a character array to lowercase
     *
     * @tparam N Size of the character array
     * @param str The input character array
     * @return std::string Lowercase version of the input string
     */
    template <std::size_t N>
    static std::string
    convert(const char (&str)[N]) {
        return std::string{toLower(str), N - 1};
    }

    /**
     * @brief Converts a string to lowercase
     *
     * @param str The input string
     * @return std::string Lowercase version of the input string
     */
    [[nodiscard]] static std::string
    convert(std::string const &str) {
        std::string copy(str.size(), '\0');
        std::transform(str.cbegin(), str.cend(), copy.begin(), charToLower);
        return copy;
    }

    /**
     * @brief Converts a string_view to lowercase
     *
     * @param str The input string_view
     * @return std::string Lowercase version of the input string
     */
    [[nodiscard]] static std::string
    convert(std::string_view const &str) {
        std::string copy(str.size(), '\0');
        std::transform(str.cbegin(), str.cend(), copy.begin(), charToLower);
        return copy;
    }
};

/**
 * @brief A case-insensitive map implementation
 *
 * This template class wraps any map type to provide case-insensitive
 * string keys by converting keys to lowercase before operations.
 *
 * @tparam _Map The underlying map type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename _Map, typename _Trait = string_to_lower>
class icase_basic_map : _Map {
    using base_t = _Map;

public:
    /** @brief Default constructor */
    icase_basic_map() = default;

    /** @brief Copy constructor */
    icase_basic_map(icase_basic_map const &) = default;

    /** @brief Move constructor */
    icase_basic_map(icase_basic_map &&) noexcept = default;

    /**
     * @brief Initializer list constructor
     *
     * @param il Initializer list of key-value pairs
     */
    icase_basic_map(std::initializer_list<typename _Map::value_type> il) {
        for (auto &it : il)
            emplace(it.first, std::move(it.second));
    }

    /** @brief Copy assignment operator */
    icase_basic_map &operator=(icase_basic_map const &) = default;

    /** @brief Move assignment operator */
    icase_basic_map &operator=(icase_basic_map &&) noexcept = default;

    /**
     * @brief Emplace a new key-value pair with the key converted to lowercase
     *
     * @tparam T Key type
     * @tparam _Args Value constructor argument types
     * @param key The key (will be converted to lowercase)
     * @param args Arguments to construct the value
     * @return Result of the underlying map's emplace operation
     */
    template <typename T, typename... _Args>
    auto
    emplace(T &&key, _Args &&...args) {
        return static_cast<base_t &>(*this).emplace(_Trait::convert(std::forward<T>(key)), std::forward<_Args>(args)...);
    }

    /**
     * @brief Try to emplace a new key-value pair with the key converted to lowercase
     *
     * @tparam T Key type
     * @tparam _Args Value constructor argument types
     * @param key The key (will be converted to lowercase)
     * @param args Arguments to construct the value
     * @return Result of the underlying map's try_emplace operation
     */
    template <typename T, typename... _Args>
    auto
    try_emplace(T &&key, _Args &&...args) {
        return static_cast<base_t &>(*this).try_emplace(_Trait::convert(std::forward<T>(key)), std::forward<_Args>(args)...);
    }

    /**
     * @brief Access a value by key, with the key converted to lowercase
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return Reference to the value associated with the key
     * @throws std::out_of_range if the key is not found
     */
    template <typename T>
    [[nodiscard]] auto &
    at(T &&key) {
        return static_cast<base_t &>(*this).at(_Trait::convert(std::forward<T>(key)));
    }

    /**
     * @brief Access a value by key, with the key converted to lowercase (const version)
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return Reference to the value associated with the key
     * @throws std::out_of_range if the key is not found
     */
    template <typename T>
    [[nodiscard]] const auto &
    at(T &&key) const {
        return static_cast<base_t const &>(*this).at(_Trait::convert(std::forward<T>(key)));
    }

    /**
     * @brief Access or insert a value by key, with the key converted to lowercase
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return Reference to the value associated with the key
     */
    template <typename T>
    auto &
    operator[](T &&key) {
        return static_cast<base_t &>(*this)[_Trait::convert(std::forward<T>(key))];
    }

    /**
     * @brief Find a key-value pair by key, with the key converted to lowercase
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return Iterator to the key-value pair if found, or end() if not found
     */
    template <typename T>
    [[nodiscard]] auto
    find(T &&key) {
        return static_cast<base_t &>(*this).find(_Trait::convert(std::forward<T>(key)));
    }

    /**
     * @brief Find a key-value pair by key, with the key converted to lowercase (const
     * version)
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return Iterator to the key-value pair if found, or end() if not found
     */
    template <typename T>
    [[nodiscard]] auto
    find(T &&key) const {
        return static_cast<base_t const &>(*this).find(_Trait::convert(std::forward<T>(key)));
    }

    /**
     * @brief Check if a key exists in the map
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return true if the key exists, false otherwise
     */
    template <typename T>
    [[nodiscard]] bool
    has(T &&key) const {
        return find(std::forward<T>(key)) != this->cend();
    }

    /**
     * @brief Erase a key-value pair by key, with the key converted to lowercase
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return The number of elements erased.
     */
    template <typename T>
    auto
    erase(T &&key) {
        return static_cast<base_t &>(*this).erase(_Trait::convert(std::forward<T>(key)));
    }

    // Import methods from the base map
    using base_t::begin;
    using base_t::cbegin;
    using base_t::cend;
    using base_t::clear;
    using base_t::empty;
    using base_t::end;
    using base_t::erase;
    using base_t::size;

    /**
     * @brief Convert a key to lowercase
     *
     * Utility method to convert a key to lowercase outside of map operations.
     *
     * @tparam T Key type
     * @param key The key to convert
     * @return std::string The lowercase version of the key
     */
    template <typename T>
    static std::string
    convert_key(T &&key) noexcept {
        return _Trait::convert(std::forward<T>(key));
    }
};

/**
 * @brief Case-insensitive ordered map using std::map
 *
 * @tparam Value The value type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename Value, typename _Trait = string_to_lower>
using icase_map = icase_basic_map<std::map<std::string, Value>, _Trait>;

/**
 * @brief Case-insensitive unordered map using qb::unordered_map
 *
 * @tparam Value The value type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename Value, typename _Trait = string_to_lower>
using icase_unordered_map = icase_basic_map<qb::unordered_map<std::string, Value>, _Trait>;

} // namespace qb

#endif // QB_UNORDERED_MAP_H
