/**
 * @file qb/system/container/unordered_set.h
 * @brief Optimized unordered set implementations
 *
 * This file provides optimized and specialized unordered set implementations
 * for the QB framework. It includes high-performance alternatives to the
 * standard unordered_set using flat hash sets from the ska_hash library.
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

#ifndef QB_UNORDERED_SET_H
#define QB_UNORDERED_SET_H
#include <set>
#include <ska_hash/unordered_map.hpp>
#include <unordered_set>

namespace qb {

/**
 * @brief A high-performance flat hash set implementation
 *
 * This is a type alias for ska::flat_hash_set which provides better performance
 * characteristics than std::unordered_set for many use cases. It uses open addressing
 * with robin hood hashing for better cache locality and performance.
 *
 * @tparam K The key type
 * @tparam H The hash function type (defaults to std::hash<K>)
 * @tparam E The equality function type (defaults to std::equal_to<K>)
 * @tparam A The allocator type
 */
template <typename K, typename H = std::hash<K>, typename E = std::equal_to<K>,
          typename A = std::allocator<K>>
using unordered_flat_set = ska::flat_hash_set<K, H, E, A>;

#ifdef NDEBUG
/**
 * @brief The primary unordered set implementation
 *
 * In release builds, this uses the high-performance ska::unordered_set
 * implementation. In debug builds, it falls back to std::unordered_set
 * for better debugging support.
 *
 * @tparam K The key type
 * @tparam H The hash function type (defaults to std::hash<K>)
 * @tparam E The equality function type (defaults to std::equal_to<K>)
 * @tparam A The allocator type
 */
template <typename K, typename H = std::hash<K>, typename E = std::equal_to<K>,
          typename A = std::allocator<K>>
using unordered_set = ska::unordered_set<K, H, E, A>;
#else
/**
 * @brief The primary unordered set implementation
 *
 * In release builds, this uses the high-performance ska::unordered_set
 * implementation. In debug builds, it falls back to std::unordered_set
 * for better debugging support.
 *
 * @tparam K The key type
 * @tparam H The hash function type (defaults to std::hash<K>)
 * @tparam E The equality function type (defaults to std::equal_to<K>)
 * @tparam A The allocator type
 * @ingroup Container
 */
template <typename K, typename H = std::hash<K>, typename E = std::equal_to<K>,
          typename A = std::allocator<K>>
using unordered_set = std::unordered_set<K, H, E, A>;
#endif

} // namespace qb

#endif // QB_UNORDERED_SET_H
