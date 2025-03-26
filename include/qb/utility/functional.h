/**
 * @file qb/utility/functional.h
 * @brief Functional utilities for hash computations
 *
 * This file provides utility functions for combining hash values of multiple objects,
 * which is useful for creating composite hash functions for custom types or
 * in containers like unordered maps and sets.
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
 * @brief Internal function to combine a single value into a hash seed
 * 
 * Uses the FNV-1a hash combining approach to mix a new value into an existing seed.
 * 
 * @tparam T Type of the value to hash
 * @param seed The seed value to combine with (modified in-place)
 * @param val The value to combine into the seed
 */
template <typename T>
void
_hash_combine(size_t &seed, const T &val) {
    seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed << 6u) + (seed >> 2u);
}

/**
 * @brief Combines the hash values of multiple objects into a single hash
 * 
 * This is particularly useful for creating hash functions for composite objects
 * or when you need to hash multiple fields together.
 * 
 * @tparam Types Parameter pack of types to hash
 * @param args The values to combine into a single hash
 * @return A hash value combining all input values
 * 
 * @example
 * struct MyStruct {
 *     int a;
 *     std::string b;
 *     
 *     struct Hash {
 *         size_t operator()(const MyStruct& s) const {
 *             return qb::hash_combine(s.a, s.b);
 *         }
 *     };
 * };
 */
template <typename... Types>
size_t
hash_combine(const Types &... args) {
    size_t seed = 0;
    (_hash_combine(seed, args), ...); // create hash value with seed over all args
    return seed;
}
} // namespace qb

#endif // QB_FUNCTIONAL_H
