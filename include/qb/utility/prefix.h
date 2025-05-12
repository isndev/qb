/**
 * @file qb/utility/prefix.h
 * @brief Platform-specific alignment macros, cache-line definitions, and related utilities.
 *
 * This file provides platform-specific macros and constants crucial for performance-sensitive code,
 * especially in lock-free algorithms and memory-efficient data structures. It defines:
 * - `QB_LOCKFREE_CACHELINE_BYTES`: The detected or assumed cache line size.
 * - `QB_LOCKFREE_EVENT_BUCKET_BYTES`: The size for event partitioning, often aligned to cache lines.
 * - `QB_LOCKFREE_PTR_COMPRESSION`: A macro indicating if pointer compression techniques might be applicable (platform-dependent).
 * - `QB_LOCKFREE_CACHELINE_ALIGNMENT`: Macro for aligning structs/classes to cache line boundaries (e.g., `alignas` or `__declspec(align)`).
 * - `QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT`: Macro for aligning to event bucket boundaries.
 * - Helper structs `CacheLine` and `EventBucket` for creating correctly sized and aligned padding or base structures.
 *
 * These are essential for optimizing memory layout to prevent false sharing and improve cache performance.
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

#ifndef QB_UTILS_PREFIX_H
#define QB_UTILS_PREFIX_H

/* this file defines the following macros:
   QB_LOCKFREE_CACHELINE_BYTES: size of a cache line
   QB_LOCKFREE_EVENT_BUCKET_BYTES: size of a event partition
   QB_LOCKFREE_PTR_COMPRESSION: use tag/pointer compression to utilize parts
                                   of the virtual address space as tag (at least 16bit)
   QB_LOCKFREE_DCAS_ALIGNMENT:  symbol used for aligning structs at cache line
                                   boundaries
*/

/**
 * @brief Determines the optimal cache line size for the current platform at compile time.
 * @ingroup SystemInfo
 * @return The cache line size in bytes.
 * @details Uses `KNOWN_L1_CACHE_LINE_SIZE` if defined, `std::hardware_destructive_interference_size`
 *          if available (C++17), or defaults to 64 bytes as a common fallback.
 *          This value is used to define `QB_LOCKFREE_CACHELINE_BYTES`.
 */
constexpr std::size_t
cache_line_size() {
#ifdef KNOWN_L1_CACHE_LINE_SIZE
    return KNOWN_L1_CACHE_LINE_SIZE;
#elif defined(__cpp_lib_hardware_interference_size)
    return std::hardware_destructive_interference_size;
#else
    return 64;
#endif
}

#define QB_LOCKFREE_CACHELINE_BYTES cache_line_size()
#define QB_LOCKFREE_EVENT_BUCKET_BYTES cache_line_size()

#ifdef _MSC_VER

#define QB_LOCKFREE_CACHELINE_ALIGNMENT __declspec(align(QB_LOCKFREE_CACHELINE_BYTES))
#define QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT \
    __declspec(align(QB_LOCKFREE_EVENT_BUCKET_BYTES))

#if defined(_M_IX86)
#define QB_LOCKFREE_DCAS_ALIGNMENT
#elif defined(_M_X64) || defined(_M_IA64)
#define QB_LOCKFREE_PTR_COMPRESSION 1
#define QB_LOCKFREE_DCAS_ALIGNMENT __declspec(align(16))
#endif

#endif /* _MSC_VER */

#ifdef __GNUC__

#define QB_LOCKFREE_CACHELINE_ALIGNMENT alignas(QB_LOCKFREE_CACHELINE_BYTES)
#define QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT alignas(QB_LOCKFREE_EVENT_BUCKET_BYTES)

#if defined(__i386__) || defined(__ppc__)
#define QB_LOCKFREE_DCAS_ALIGNMENT
#elif defined(__x86_64__)
#define QB_LOCKFREE_PTR_COMPRESSION 1
#define QB_LOCKFREE_DCAS_ALIGNMENT __attribute__((aligned(16)))
#elif defined(__alpha__)
#define QB_LOCKFREE_PTR_COMPRESSION 1
#define QB_LOCKFREE_DCAS_ALIGNMENT
#endif
#endif /* __GNUC__ */

/**
 * @struct CacheLine
 * @ingroup System
 * @brief A structure automatically aligned to cache line boundaries.
 *
 * This structure is padded to occupy exactly one cache line (`QB_LOCKFREE_CACHELINE_BYTES`).
 * It can be used as a base class or member to ensure that an object starts on a cache line
 * boundary, which can help prevent false sharing in concurrent applications when different
 * threads access adjacent data that might otherwise fall into the same cache line.
 * Contains a raw array `__raw__` for padding purposes.
 */
struct QB_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
    uint32_t __raw__[QB_LOCKFREE_CACHELINE_BYTES / sizeof(uint32_t)];
};

/**
 * @struct EventBucket
 * @ingroup System
 * @brief A structure aligned to event bucket boundaries, typically matching cache line size.
 *
 * This structure is padded to `QB_LOCKFREE_EVENT_BUCKET_BYTES` (often same as cache line size).
 * It is used in event queues or allocators to ensure that event objects or their containers
 * are aligned in memory, potentially improving performance by optimizing cache usage and
 * reducing contention in concurrent scenarios involving event processing.
 * Contains a raw array `__raw__` for padding.
 */
struct QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT EventBucket {
    uint32_t __raw__[QB_LOCKFREE_EVENT_BUCKET_BYTES / sizeof(uint32_t)];
};

#endif /* QB_UTILS_PREFIX_H */
