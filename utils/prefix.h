
#ifndef CUBE_UTILS_PREFIX_H
#define CUBE_UTILS_PREFIX_H

/* this file defines the following macros:
   CUBE_LOCKFREE_CACHELINE_BYTES: size of a cache line
   CUBE_LOCKFREE_PTR_COMPRESSION: use tag/pointer compression to utilize parts
                                   of the virtual address space as tag (at least 16bit)
   CUBE_LOCKFREE_DCAS_ALIGNMENT:  symbol used for aligning structs at cache line
                                   boundaries
*/

#define CUBE_LOCKFREE_CACHELINE_BYTES 64

#ifdef _MSC_VER

#define CUBE_LOCKFREE_CACHELINE_ALIGNMENT __declspec(align(CUBE_LOCKFREE_CACHELINE_BYTES))

#if defined(_M_IX86)
#define CUBE_LOCKFREE_DCAS_ALIGNMENT
#elif defined(_M_X64) || defined(_M_IA64)
#define CUBE_LOCKFREE_PTR_COMPRESSION 1
#define CUBE_LOCKFREE_DCAS_ALIGNMENT __declspec(align(16))
#endif

#endif /* _MSC_VER */

#ifdef __GNUC__

#define CUBE_LOCKFREE_CACHELINE_ALIGNMENT alignas(CUBE_LOCKFREE_CACHELINE_BYTES)
//__attribute__((aligned(CUBE_LOCKFREE_CACHELINE_BYTES)))

#if defined(__i386__) || defined(__ppc__)
#define CUBE_LOCKFREE_DCAS_ALIGNMENT
#elif defined(__x86_64__)
#define CUBE_LOCKFREE_PTR_COMPRESSION 1
#define CUBE_LOCKFREE_DCAS_ALIGNMENT __attribute__((aligned(16)))
#elif defined(__alpha__)
#define CUBE_LOCKFREE_PTR_COMPRESSION 1
#define CUBE_LOCKFREE_DCAS_ALIGNMENT
#endif
#endif /* __GNUC__ */

#endif /* CUBE_UTILS_PREFIX_H */
