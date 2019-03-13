/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QB_UTILS_PREFIX_H
#define QB_UTILS_PREFIX_H

/* this file defines the following macros:
   QB_LOCKFREE_CACHELINE_BYTES: size of a cache line
   QB_LOCKFREE_PTR_COMPRESSION: use tag/pointer compression to utilize parts
                                   of the virtual address space as tag (at least 16bit)
   QB_LOCKFREE_DCAS_ALIGNMENT:  symbol used for aligning structs at cache line
                                   boundaries
*/

#define QB_LOCKFREE_CACHELINE_BYTES 64

#ifdef _MSC_VER

#define QB_LOCKFREE_CACHELINE_ALIGNMENT __declspec(align(QB_LOCKFREE_CACHELINE_BYTES))

#if defined(_M_IX86)
#define QB_LOCKFREE_DCAS_ALIGNMENT
#elif defined(_M_X64) || defined(_M_IA64)
#define QB_LOCKFREE_PTR_COMPRESSION 1
#define QB_LOCKFREE_DCAS_ALIGNMENT __declspec(align(16))
#endif

#endif /* _MSC_VER */

#ifdef __GNUC__

#define QB_LOCKFREE_CACHELINE_ALIGNMENT alignas(QB_LOCKFREE_CACHELINE_BYTES)
//__attribute__((aligned(QB_LOCKFREE_CACHELINE_BYTES)))

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

struct QB_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
    uint32_t __raw__[16];
};

#endif /* QB_UTILS_PREFIX_H */
