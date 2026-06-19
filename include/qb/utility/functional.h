/**
 * @file qb/utility/functional.h
 * @brief Functional utilities, primarily for hash computations.
 *
 * This file provides utility functions for combining hash values of multiple objects,
 * which is useful for creating composite hash functions for custom types or
 * for use in containers like `qb::unordered_map` and `qb::unordered_set` when a custom
 * hasher for a key type is needed.
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
 * @ingroup Utility
 */

#ifndef QB_FUNCTIONAL_H
#define QB_FUNCTIONAL_H
#include <cstddef>
#include <functional>

namespace qb {

namespace detail {

/**
 * @brief Combines a single value's hash into a seed (FNV-1a inspired).
 * @tparam T Type of the value to hash.
 * @param seed The running hash seed (modified in-place).
 * @param val The value whose hash is to be folded into the seed.
 */
template <typename T>
constexpr void
hash_combine_one(std::size_t &seed, const T &val) noexcept {
    seed ^= std::hash<T>{}(val) + 0x9e3779b9 + (seed << 6u) + (seed >> 2u);
}

} // namespace detail

/**
 * @brief Combines the hash values of multiple objects into a single hash value.
 * @ingroup MiscUtils
 * @tparam Types Variadic template parameter pack of the types of objects to hash.
 * @param args The values whose hash codes are to be combined.
 * @return A single `size_t` hash value representing the combination of all input values.
 *
 * @code
 * struct MyKey {
 *     int id;
 *     std::string name;
 *
 *     bool operator==(const MyKey&) const = default;
 * };
 *
 * namespace std {
 *   template <>
 *   struct hash<MyKey> {
 *     std::size_t operator()(const MyKey& k) const {
 *       return qb::hash_combine(k.id, k.name);
 *     }
 *   };
 * }
 * @endcode
 */
template <typename... Types>
[[nodiscard]] constexpr std::size_t
hash_combine(const Types &...args) noexcept {
    std::size_t seed = 0;
    (detail::hash_combine_one(seed, args), ...);
    return seed;
}

} // namespace qb

#endif // QB_FUNCTIONAL_H
