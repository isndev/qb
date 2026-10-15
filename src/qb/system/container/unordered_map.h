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
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <qb/vendor/ska_hash/unordered_map.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

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
 *          otherwise (added 2020-03-30 in e88a597c, with no recorded rationale beyond
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
 * @note **`contains()` is a qb addition to the fork.** Upstream `ska::unordered_map`
 *       predates C++20 and has no such member, which made this the one place the drop-in
 *       promise above failed — a caller reaching for the obvious spelling got a compile
 *       error naming a template deep inside the vendored header. `qb/vendor/ska_hash/` is
 *       a qb fork, not a verbatim copy (it already carries a local fix for a shared empty
 *       sentinel), and the vendor-attribution contract requires the license to be present,
 *       installable and recorded — not the source to be untouched — so the member was
 *       added rather than documented away. It is scoped exactly like `count()`: same key
 *       type, same lookup, same O(1), and **no heterogeneous overload**, because `find()`
 *       has none either and adding one would out-promise the container.
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
 * Provides the ASCII case folding the case-insensitive maps key on: `fold()` lowercases bytes
 * in place or into a caller's buffer, eight at a time; the `convert()` overloads return the
 * folded copy as a `std::string`.
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

    /// Eight bytes with every `'A'`..`'Z'` lowercased and every other byte left alone.
    [[nodiscard]] static constexpr std::uint64_t
    fold_word(std::uint64_t w) noexcept {
        constexpr std::uint64_t ones = 0x0101010101010101ULL;
        // Bit 7 is cleared first so the two additions never carry into the next byte; a byte
        // that had it set is not an ASCII letter and is excluded by `~w` below.
        const std::uint64_t lo       = w & (ones * 0x7F);
        const std::uint64_t ge_a     = lo + ones * (0x80 - 'A');     // bit 7 set iff byte >= 'A'
        const std::uint64_t gt_z     = lo + ones * (0x80 - 'Z' - 1); // bit 7 set iff byte > 'Z'
        const std::uint64_t is_upper = ge_a & ~gt_z & ~w & (ones * 0x80);
        return w | (is_upper >> 2); // 0x80 >> 2 == 0x20: the case bit
    }

public:
    /**
     * @brief Lowercases `n` bytes of `src` into `dst`, ASCII only (`'A'`..`'Z'`; every other
     *        byte is copied unchanged).
     *
     * Eight bytes per step, the last eight re-read overlapped (folding is idempotent), a byte
     * loop under eight. `dst` may be `src` itself (in place) or a non-overlapping buffer.
     *
     * @param src The bytes to fold.
     * @param dst Where the folded bytes go; at least `n` bytes.
     * @param n   How many bytes.
     */
    static void
    fold(const char *src, char *dst, std::size_t n) noexcept {
        if (n < sizeof(std::uint64_t)) {
            for (std::size_t i = 0; i < n; ++i)
                dst[i] = charToLower(src[i]);
            return;
        }
        std::size_t i = 0;
        for (; i + sizeof(std::uint64_t) <= n; i += sizeof(std::uint64_t)) {
            std::uint64_t w;
            std::memcpy(&w, src + i, sizeof w);
            w = fold_word(w);
            std::memcpy(dst + i, &w, sizeof w);
        }
        if (i < n) {
            std::uint64_t w;
            std::memcpy(&w, src + n - sizeof w, sizeof w);
            w = fold_word(w);
            std::memcpy(dst + n - sizeof w, &w, sizeof w);
        }
    }

    /**
     * @brief Converts a character array to lowercase
     *
     * @tparam N Size of the character array; `N - 1` bytes are converted (a string literal).
     * @param str The input character array
     * @return std::string Lowercase version of the input string
     */
    template <std::size_t N>
    static std::string
    convert(const char (&str)[N]) {
        std::string copy(str, N - 1);
        fold(copy.data(), copy.data(), N - 1);
        return copy;
    }

    /**
     * @brief Converts a string to lowercase
     *
     * @param str The input string
     * @return std::string Lowercase version of the input string
     */
    [[nodiscard]] static std::string
    convert(std::string const &str) {
        std::string copy(str);
        fold(copy.data(), copy.data(), copy.size());
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
        std::string copy(str);
        fold(copy.data(), copy.data(), copy.size());
        return copy;
    }
};

/**
 * @brief The transparent string hash the case-insensitive maps key on
 *
 * Hashes any string-like key through `std::hash<std::string_view>`, whose value the standard
 * requires to equal `std::hash<std::string>` of the same bytes ([basic.string.hash]): a map that
 * switches to it keeps every bucket, and every iteration order, it had with `std::hash<std::string>`,
 * and gains lookups that take a `std::string_view` without building a `std::string` first.
 */
struct string_hash {
    using is_transparent = void;

    [[nodiscard]] std::size_t
    operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

/**
 * @brief A case-insensitive map implementation
 *
 * This template class wraps any map type to provide case-insensitive
 * string keys by converting keys to lowercase before operations.
 *
 * The stored key is the folded one (`_Trait::convert`), built once, when a key is INSERTED
 * (`emplace`, `try_emplace`, `operator[]`). A lookup (`find`, `at`, `has`, `erase(key)`) builds
 * nothing: when the base map accepts a `std::string_view` key and the trait offers `fold()`,
 * the key is folded into a buffer on the stack and looked up as a view -- no `std::string`, no
 * allocation, whatever the key's length -- and only a key longer than that buffer goes through
 * `convert()`. A base map without heterogeneous lookup, or a trait without `fold()`, keeps the
 * converting path for everything.
 *
 * @tparam _Map The underlying map type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename _Map, typename _Trait = string_to_lower>
class icase_basic_map : _Map {
    using base_t = _Map;

    /// The stack buffer a lookup key is folded into; longer keys are converted instead.
    static constexpr std::size_t inline_fold_capacity = 128;

    /// Whether the base map looks a `std::string_view` up without a `std::string`
    /// (a transparent hash and equality, or a transparent comparator).
    static constexpr bool heterogeneous_lookup = requires(const base_t &m, std::string_view v) { m.find(v); };

    /// Whether the trait folds into a caller's buffer.
    static constexpr bool trait_folds = requires(const char *s, char *d, std::size_t n) { _Trait::fold(s, d, n); };

    /// The bytes of a key, as a view: `N - 1` for a character array (a string literal),
    /// `std::string_view` for anything that converts to one.
    template <std::size_t N>
    [[nodiscard]] static constexpr std::string_view
    key_view(const char (&key)[N]) noexcept {
        return {key, N - 1};
    }
    template <typename T>
    requires(!std::is_array_v<std::remove_cvref_t<T>> && std::is_convertible_v<const T &, std::string_view>)
    [[nodiscard]] static constexpr std::string_view
    key_view(const T &key) noexcept(std::is_nothrow_convertible_v<const T &, std::string_view>) {
        return std::string_view{key};
    }

    /// Whether a key of type `T` can take the stack-folded path.
    template <typename T>
    static constexpr bool viewable_key = requires(const T &key) { key_view(key); };

    /**
     * @brief Runs `op` on the folded form of `key`: a view of a stack buffer when the base map
     *        can look a view up and the key fits, the converted `std::string` otherwise.
     */
    template <typename T, typename Op>
    static decltype(auto)
    with_folded_key(T &&key, Op &&op) {
        if constexpr (heterogeneous_lookup && trait_folds && viewable_key<std::remove_cvref_t<T>>) {
            const std::string_view raw = key_view(key);
            if (raw.size() <= inline_fold_capacity) {
                char folded[inline_fold_capacity];
                _Trait::fold(raw.data(), folded, raw.size());
                return op(std::string_view{folded, raw.size()});
            }
            return op(_Trait::convert(raw));
        } else {
            return op(_Trait::convert(std::forward<T>(key)));
        }
    }

public:
    /** @brief Default constructor */
    icase_basic_map() = default;

    /** @brief Copy constructor */
    icase_basic_map(icase_basic_map const &) = default;

    /** @brief Move constructor: as nothrow as the base map's own -- MSVC's `std::map` allocates its sentinel in its
     *         move constructor, and a move that cannot throw would terminate on a full heap (Huly QB-1074). */
    icase_basic_map(icase_basic_map &&) noexcept(std::is_nothrow_move_constructible_v<_Map>) = default;

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

    /** @brief Move assignment operator: as nothrow as the base map's own (Huly QB-1074). */
    icase_basic_map &operator=(icase_basic_map &&) noexcept(std::is_nothrow_move_assignable_v<_Map>) = default;

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
        auto it = find(std::forward<T>(key));
        if (it == static_cast<base_t &>(*this).end())
            throw std::out_of_range("Argument passed to at() was not in the map.");
        return it->second;
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
        auto it = find(std::forward<T>(key));
        if (it == static_cast<base_t const &>(*this).end())
            throw std::out_of_range("Argument passed to at() was not in the map.");
        return it->second;
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
        return with_folded_key(std::forward<T>(key), [this](auto const &folded) { return static_cast<base_t &>(*this).find(folded); });
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
        return with_folded_key(std::forward<T>(key), [this](auto const &folded) { return static_cast<base_t const &>(*this).find(folded); });
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
     * Every key form is folded, a `const std::string &` included. The base's own
     * `erase(const key_type &)` is deliberately not exposed: for a `const std::string` lvalue
     * it ties with this template, wins as the non-template, and looks the UNFOLDED key up in
     * folded storage (Huly QB-279). Only keys the trait can convert select this overload, so
     * iterators reach the position forms below.
     *
     * @tparam T Key type
     * @param key The key (will be converted to lowercase)
     * @return The number of elements erased.
     */
    template <typename T>
    requires requires(T &&key) { _Trait::convert(std::forward<T>(key)); }
    std::size_t
    erase(T &&key) {
        return with_folded_key(std::forward<T>(key), [this](auto const &folded) -> std::size_t {
            auto      &base = static_cast<base_t &>(*this);
            const auto it   = base.find(folded);
            if (it == base.end())
                return 0;
            base.erase(it);
            return 1;
        });
    }

    /**
     * @brief Erase the element at a position (an `iterator` converts)
     * @param pos A valid, dereferenceable position of this map.
     * @return What the base map's position `erase` returns.
     */
    auto
    erase(typename base_t::const_iterator pos) {
        return static_cast<base_t &>(*this).erase(pos);
    }

    /**
     * @brief Erase the elements of the range [first, last)
     * @param first The first position to erase.
     * @param last The position after the last one to erase.
     * @return What the base map's range `erase` returns.
     */
    auto
    erase(typename base_t::const_iterator first, typename base_t::const_iterator last) {
        return static_cast<base_t &>(*this).erase(first, last);
    }

    // Import methods from the base map (`erase` is spelled out above, never imported)
    using base_t::begin;
    using base_t::cbegin;
    using base_t::cend;
    using base_t::clear;
    using base_t::empty;
    using base_t::end;
    using base_t::size;

    /**
     * @brief Convert a key to lowercase
     *
     * Utility method to convert a key to lowercase outside of map operations.
     *
     * @tparam T Key type
     * @param key The key to convert
     * @return std::string The lowercase version of the key
     * @throws std::bad_alloc (or whatever `_Trait::convert` throws) when the folded `std::string` cannot be
     *         allocated. Not `noexcept`: it was until 3.3, which turned that failure into `std::terminate()` (Huly
     *         QB-1074). A caller that must not allocate folds into its own buffer, as a lookup does
     *         (`lookup_fold_capacity`).
     */
    template <typename T>
    static std::string
    convert_key(T &&key) {
        return _Trait::convert(std::forward<T>(key));
    }

    /**
     * @brief The longest key a lookup (`find`, `has`, `at`, `erase(key)`) folds on the stack: such a
     *        lookup builds no `std::string` and allocates nothing. A longer key is converted, which
     *        allocates; 0 when this map converts every key (a base map without heterogeneous lookup,
     *        or a trait without `fold()`). A caller that must not allocate -- a `noexcept` lookup --
     *        takes another path past it (qbm-http's `Headers::has_header` / `remove_header`, Huly QB-1068).
     */
    static constexpr std::size_t lookup_fold_capacity = heterogeneous_lookup && trait_folds ? inline_fold_capacity : 0;
};

/**
 * @brief Case-insensitive ordered map using std::map
 *
 * The comparator is transparent (`std::less<>`), so a lookup takes its folded key as a view.
 *
 * @tparam Value The value type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename Value, typename _Trait = string_to_lower>
using icase_map = icase_basic_map<std::map<std::string, Value, std::less<>>, _Trait>;

/**
 * @brief Case-insensitive unordered map using qb::unordered_map
 *
 * Keyed on `qb::string_hash` and `std::equal_to<>`, both transparent, so a lookup takes its
 * folded key as a view; the hash values are those of `std::hash<std::string>`.
 *
 * @tparam Value The value type
 * @tparam _Trait The trait class for string conversion (defaults to string_to_lower)
 */
template <typename Value, typename _Trait = string_to_lower>
using icase_unordered_map = icase_basic_map<qb::unordered_map<std::string, Value, string_hash, std::equal_to<>>, _Trait>;

} // namespace qb

#endif // QB_UNORDERED_MAP_H
