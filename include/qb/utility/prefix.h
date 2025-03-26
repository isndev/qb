/**
 * @file qb/utility/prefix.h
 * @brief Platform-specific alignment and cache-line definitions
 *
 * This file provides platform-specific macros and constants for cache-line
 * alignment, event bucket sizes, and pointer compression techniques.
 * These are essential for high-performance lockfree algorithms and
 * memory-efficient data structures.
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
 * @brief Determines the optimal cache line size for the current platform
 * 
 * Uses compile-time detection or defaults to 64 bytes if the actual
 * cache line size cannot be determined.
 * 
 * @return The cache line size in bytes
 */
constexpr std::size_t cache_line_size() {
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

#    define QB_LOCKFREE_CACHELINE_ALIGNMENT \
        __declspec(align(QB_LOCKFREE_CACHELINE_BYTES))
#    define QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT \
        __declspec(align(QB_LOCKFREE_EVENT_BUCKET_BYTES))

#    if defined(_M_IX86)
#        define QB_LOCKFREE_DCAS_ALIGNMENT
#    elif defined(_M_X64) || defined(_M_IA64)
#        define QB_LOCKFREE_PTR_COMPRESSION 1
#        define QB_LOCKFREE_DCAS_ALIGNMENT __declspec(align(16))
#    endif

#endif /* _MSC_VER */

#ifdef __GNUC__

#    define QB_LOCKFREE_CACHELINE_ALIGNMENT alignas(QB_LOCKFREE_CACHELINE_BYTES)
#    define QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT alignas(QB_LOCKFREE_EVENT_BUCKET_BYTES)

#    if defined(__i386__) || defined(__ppc__)
#        define QB_LOCKFREE_DCAS_ALIGNMENT
#    elif defined(__x86_64__)
#        define QB_LOCKFREE_PTR_COMPRESSION 1
#        define QB_LOCKFREE_DCAS_ALIGNMENT __attribute__((aligned(16)))
#    elif defined(__alpha__)
#        define QB_LOCKFREE_PTR_COMPRESSION 1
#        define QB_LOCKFREE_DCAS_ALIGNMENT
#    endif
#endif /* __GNUC__ */

/**
 * @brief Structure aligned to cache line boundaries
 * 
 * This structure is padded to the size of a full cache line to prevent
 * false sharing between adjacent structures in memory.
 */
struct QB_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
    uint32_t __raw__[QB_LOCKFREE_CACHELINE_BYTES / sizeof(uint32_t)];
};

/**
 * @brief Structure aligned to event bucket boundaries
 * 
 * This structure is padded to the size of a full event bucket to optimize
 * event processing and prevent contention between event handlers.
 */
struct QB_LOCKFREE_EVENT_BUCKET_ALIGNMENT EventBucket {
    uint32_t __raw__[QB_LOCKFREE_EVENT_BUCKET_BYTES / sizeof(uint32_t)];
};

#endif /* QB_UTILS_PREFIX_H */
