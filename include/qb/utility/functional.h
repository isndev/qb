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

#ifndef QB_FUNCTIONAL_H
#define QB_FUNCTIONAL_H
#include <functional>

namespace qb {

/**
 * @brief Internal helper function to combine a single value into a hash seed.
 * @private
 * @tparam T Type of the value to hash.
 * @param seed The seed value to combine with (modified in-place by XORing and bit manipulation).
 * @param val The value whose hash is to be combined into the seed.
 * @details Uses a common hash combining approach similar to FNV-1a or boost::hash_combine.
 */
template <typename T>
void
_hash_combine(size_t &seed, const T &val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6u) + (seed >> 2u);
}

/**
 * @brief Combines the hash values of multiple objects into a single hash value.
 * @ingroup MiscUtils
 * @tparam Types Variadic template parameter pack of the types of objects to hash.
 * @param args The values whose hash codes are to be combined.
 * @return A single `size_t` hash value representing the combination of all input values.
 * @details This function is particularly useful for creating custom hash functions for composite
 *          objects (structs or classes) to be used as keys in hash-based containers like
 *          `qb::unordered_map` or `std::unordered_map`. It iteratively combines the hash
 *          of each argument into a seed.
 *
 * @code
 * struct MyKey {
 *     int id;
 *     std::string name;
 *     double value;
 *
 *     bool operator==(const MyKey& other) const {
 *         return id == other.id && name == other.name && value == other.value;
 *     }
 * };
 *
 * namespace std {
 *   template <>
 *   struct hash<MyKey> {
 *     std::size_t operator()(const MyKey& k) const {
 *       return qb::hash_combine(k.id, k.name, k.value);
 *     }
 *   };
 * } // namespace std
 *
 * // ... later ...
 * // qb::unordered_map<MyKey, SomeData> my_map;
 * @endcode
 */
template <typename... Types>
size_t
hash_combine(const Types &...args) {
    size_t seed = 0;
    (_hash_combine(seed, args), ...); // create hash value with seed over all args
    return seed;
}
} // namespace qb

#endif // QB_FUNCTIONAL_H
