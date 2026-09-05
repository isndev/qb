/**
 * @file qb/system/allocator/slab.h
 * @brief Process-wide cache of 2 MB memory slabs, obtained from the platform and kept warm
 *
 * `slab_cache` is where `segment_pool` gets the memory it carves event-pipe segments from. It
 * exists because of one measurement (qb-vs-others finding 9.11, then its A/B): with each 256 KB
 * segment taken from `malloc`, a 1 M-event one-core burst faulted **15 640 pages per run** —
 * every segment, 4 KB at a time, on every fresh engine — and on WSL2 a minor fault costs ~1 µs,
 * so that alone was 15 ns of the 20 ns cell. The pipe it replaced looked warm only by accident:
 * its 32 MB and 64 MB rungs had pushed glibc's dynamic `mmap`/`trim` thresholds high enough that
 * the smaller rungs came back from the heap already faulted. Nothing in the segmented design
 * needed that accident; it needed two deliberate things, and this file is both:
 *
 *  - **slabs, not segments, are what the platform hands out.** A slab is 2 MB, aligned to 2 MB
 *    on POSIX, which is exactly what makes it a transparent-huge-page candidate: on Linux it is
 *    `madvise(MADV_HUGEPAGE)`d and then `MADV_POPULATE_WRITE`d, so the whole slab is faulted in
 *    ONE kernel pass — 1 fault per 2 MB instead of 512, with the zeroing batched — where the
 *    kernel supports it (5.14+; older kernels fall through to ordinary first-touch faults, and
 *    a kernel with THP set to `never` still gets the populate). macOS gets the aligned mapping,
 *    Windows a committed `VirtualAlloc` region;
 *  - **slabs outlive the pool that used them.** A released slab goes to a process-wide free list,
 *    never back to the OS, so the second engine a process starts — or the next repetition of a
 *    benchmark — draws memory that is already mapped and already faulted. That is precisely
 *    what `malloc` gives every per-message allocator and what the segmented pipe had given up.
 *    `trim()` returns the cached slabs to the OS for the caller that wants its footprint back.
 *
 * The cache is cold-path only. A `segment_pool` touches it when a pipe grows PAST the pool's
 * high water (a slab is 8 standard segments, so one acquire covers eight growths), on
 * `shrink()`, and at destruction; the per-event path never sees it. Its lock guards a few
 * pointer writes and BLOCKS rather than spins — every core of a starting engine reaches it at
 * once, and a pure spin starves the holder under ThreadSanitizer — its state is trivially
 * destructible (no static-destruction order to get wrong for a pool torn down late), and it
 * never throws except `std::bad_alloc` from `acquire()` when the platform refuses a mapping.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - C++ Actor Framework (cpp.actor)
 * @ingroup Container
 */

#ifndef QB_ALLOCATOR_SLAB_H
#define QB_ALLOCATOR_SLAB_H

#include <cstddef>

namespace qb::allocator {

/**
 * @class slab_cache
 * @brief Process-wide free list of 2 MB slabs, backed by the platform's page allocator.
 *
 * All members are static: there is one cache per process, shared by every `segment_pool` of
 * every engine the process ever starts. Thread-safe.
 */
class slab_cache {
public:
    /// Bytes per slab. Eight standard 256 KB event-pipe segments; one transparent huge page.
    static constexpr std::size_t slab_bytes = 2u * 1024u * 1024u;

    /**
     * @brief Take a slab: the most recently released one if any, a fresh mapping otherwise.
     * @return A `slab_bytes`-byte region, aligned to at least `alignment()`, whose content is
     *         unspecified (a reused slab holds whatever its last owner left).
     * @throws std::bad_alloc when the platform refuses the mapping.
     */
    [[nodiscard]] static void *acquire();

    /**
     * @brief Give a slab back to the cache. It stays mapped and warm for the next `acquire()`.
     * @param slab A pointer previously returned by `acquire()` and not released since.
     */
    static void release(void *slab) noexcept;

    /**
     * @brief Return every cached (released, unlent) slab to the OS.
     * @return The number of slabs unmapped.
     */
    static std::size_t trim() noexcept;

    /// Slabs sitting on the free list: mapped, warm, lent to nobody.
    [[nodiscard]] static std::size_t cached() noexcept;

    /// Slabs currently mapped by this cache, cached or lent.
    [[nodiscard]] static std::size_t mapped() noexcept;

    /// Slabs ever obtained from the platform by this cache (a cache hit does not count).
    [[nodiscard]] static std::size_t mappings() noexcept;

    /**
     * @brief The alignment every slab satisfies on this platform.
     *
     * `slab_bytes` on POSIX (the mapping is trimmed to a 2 MB boundary, which is what a huge
     * page needs); the allocation granularity — 64 KB — on Windows, where nothing is gained by
     * over-mapping to align further.
     */
    [[nodiscard]] static std::size_t alignment() noexcept;

    /**
     * @brief Whether the last fresh mapping was prefaulted by the kernel in one pass
     *        (`MADV_POPULATE_WRITE` accepted).
     *
     * Diagnostic: false on a kernel older than 5.14, on macOS and on Windows, where a slab is
     * faulted on first touch instead. Never affects correctness.
     */
    [[nodiscard]] static bool prefaulted() noexcept;
};

} // namespace qb::allocator

#endif // QB_ALLOCATOR_SLAB_H
