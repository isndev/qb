/**
 * @file qb/system/allocator/region.h
 * @brief Process-wide cache of page-granular memory regions: the per-core buffers one engine
 *        leaves behind, mapped and warm for the next one
 *
 * `region_cache` is where the buffers an engine builds per core, and whose size grows with the
 * engine, take their memory: the router's handler tables (one per event type, indexed by actor
 * id), the actor registry, the kill queues, the broadcast snapshots, the receive buffer and the
 * mailbox rings. It exists because of one regression and the margin its fix relied on (Huly
 * QB-1009, QB-1012). On glibc those buffers lived in `malloc`, and `malloc` decides on its own
 * whether a freed block goes back to the OS:
 *
 *  - a block at or above the `mmap` threshold is mapped for itself and UNMAPPED when freed, and
 *    the free raises the threshold to the block's size and the trim threshold to twice that --
 *    process-wide, for every allocation that follows. A 40 000-actor core's table for one event
 *    type is 65 536 slots of 32 bytes, 2 MiB: its free at the first engine's stop sets the trim
 *    threshold to 4 202 496 bytes, the threshold QB-1009 measured;
 *  - a block below the threshold lives in the thread's arena, and when a free merges the arena's
 *    top chunk past the trim threshold, glibc gives the top back with `madvise(MADV_DONTNEED)`.
 *    From the second engine on the tables lived there, and at every core-thread exit they were
 *    freed into that top: whatever pushed it past the threshold -- the thread arena's 64 KiB
 *    first chunk in QB-1009, any second event type, a larger population -- released megabytes
 *    that the next engine re-faulted page by page inside its first measured window.
 *
 * Neither threshold is a library's to set: `mallopt(M_TRIM_THRESHOLD, ...)` and its siblings are
 * process-wide, switch the dynamic adjustment off for every allocation of the program, override
 * the user's own `mallopt` or `GLIBC_TUNABLES` choice, and exist on glibc only. So the engine's
 * buffers no longer go through `malloc` at all:
 *
 *  - **a region is a platform mapping** -- `mmap` on POSIX, `VirtualAlloc` reserve+commit on
 *    Windows -- of the request rounded up to `page_bytes`. It is faulted on first touch, like
 *    the `malloc` block it replaces, and never touched again by the allocator: no header while
 *    it is lent, so a page-aligned request stays page-aligned;
 *  - **a released region stays mapped**, on a process-wide free list, and the next request of
 *    the same rounded size takes the most recently released one: the next engine rebuilds its
 *    tables in pages that are already faulted. Sizes repeat by construction -- a container that
 *    doubles asks for the same ladder of sizes every time, a mailbox for `N` cores for `N` rings;
 *  - **the retention is bounded**: the cache never holds more than twice the most the process
 *    ever had lent at once (`peak_bytes()`), which is the most a container growing by doubling
 *    can leave behind, so a process that restarts the same engine keeps all of it and one that
 *    changes shape keeps a bounded part; past the bound the regions released longest ago are
 *    unmapped first. `trim()` unmaps everything cached and forgets the high water: the call for
 *    a caller that wants its footprint back, which nothing makes for it (the same contract as
 *    `slab_cache::trim()`, `slab.h`).
 *
 * `region_allocator<T>` is the standard-library face of it: a request of at least `min_bytes`
 * is a region, a smaller one goes to the global allocator -- below 64 KiB a block neither
 * reaches glibc's thresholds by itself nor is worth a mapping, and 64 KiB is Windows' allocation
 * granularity. The cache is cold-path only: a container reaches it when it grows or dies, never
 * per event, and its lock blocks rather than spins (the reason is `slab.cpp`'s). Its state is
 * trivially destructible, so a container destroyed during static destruction, or by a
 * thread-local destructor at thread exit, still finds it. Never throws except `std::bad_alloc`
 * from `acquire()` when the platform refuses a mapping.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - C++ Actor Framework (cpp.actor)
 * @ingroup Container
 */

#ifndef QB_ALLOCATOR_REGION_H
#define QB_ALLOCATOR_REGION_H

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace qb::allocator {

/**
 * @class region_cache
 * @brief Process-wide free list of page-granular regions obtained from the platform, kept
 *        mapped across their owners and reused by exact rounded size.
 *
 * All members are static: one cache per process, shared by every engine it ever starts.
 * Thread-safe. The file comment says why it exists and what it keeps.
 */
class region_cache {
public:
    /// The smallest request `region_allocator` sends here; smaller ones go to the global allocator.
    static constexpr std::size_t min_bytes = 64u * 1024u;
    /// The unit a region's size is rounded up to: two requests that round alike share regions.
    static constexpr std::size_t page_bytes = 4096u;

    /// `bytes` rounded up to `page_bytes` (at least one page): the size a region really has.
    [[nodiscard]] static constexpr std::size_t
    rounded(std::size_t const bytes) noexcept {
        return bytes <= page_bytes ? page_bytes : (bytes + page_bytes - 1) / page_bytes * page_bytes;
    }

    /**
     * @brief Take a region of at least `bytes` bytes: the most recently released one of the same
     *        rounded size if any, a fresh mapping otherwise.
     * @return A page-aligned region whose content is unspecified (a reused region holds whatever
     *         its last owner left; a fresh one is zero).
     * @throws std::bad_alloc when the platform refuses the mapping.
     */
    [[nodiscard]] static void *acquire(std::size_t bytes);

    /**
     * @brief Give a region back. It stays mapped for the next `acquire()` of its size, unless the
     *        cache is past its bound (the file comment), which unmaps the oldest first.
     * @param region A pointer `acquire(bytes)` returned and not released since; null is ignored.
     * @param bytes  The size it was acquired with.
     */
    static void release(void *region, std::size_t bytes) noexcept;

    /**
     * @brief Return every cached (released, unlent) region to the OS, and reset the high water to
     *        what is lent now.
     * @return The number of regions unmapped.
     */
    static std::size_t trim() noexcept;

    /// Regions sitting on the free list: mapped, lent to nobody.
    [[nodiscard]] static std::size_t cached() noexcept;
    /// Bytes of the regions sitting on the free list.
    [[nodiscard]] static std::size_t cached_bytes() noexcept;
    /// Bytes of the regions currently lent.
    [[nodiscard]] static std::size_t lent_bytes() noexcept;
    /// The most bytes ever lent at once (since the last `trim()`): half the cache's bound.
    [[nodiscard]] static std::size_t peak_bytes() noexcept;
    /// Regions ever obtained from the platform (a cache hit does not count).
    [[nodiscard]] static std::size_t mappings() noexcept;
};

/**
 * @class region_allocator
 * @brief A stateless standard allocator: requests of at least `region_cache::min_bytes` are
 *        regions, smaller ones the global allocator's.
 * @details Every instance compares equal. The global fall-through is given back through the
 *          UNSIZED delete -- the sized one is absent under `-fno-sized-deallocation`, an axis
 *          qb's ABI fingerprint builds -- and an over-aligned `T` takes the aligned forms.
 *          `deallocate(p, n)` must get the `n` its `allocate` got, as every standard container
 *          passes it. `T` may be incomplete until `allocate` is used, as the standard asks of an
 *          allocator a `std::vector` member can name.
 * @tparam T The element type; its alignment may not exceed a page.
 */
template <typename T>
class region_allocator {
public:
    using value_type = T;

    region_allocator() noexcept = default;
    template <typename U>
    constexpr region_allocator(region_allocator<U> const &) noexcept {}

    [[nodiscard]] T *
    allocate(std::size_t const n) {
        static_assert(alignof(T) <= region_cache::page_bytes, "a region is page-aligned, no more");
        if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T))
            throw std::bad_array_new_length();
        std::size_t const bytes = n * sizeof(T);
        if (bytes >= region_cache::min_bytes)
            return static_cast<T *>(region_cache::acquire(bytes));
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return static_cast<T *>(::operator new(bytes, std::align_val_t{alignof(T)}));
        else
            return static_cast<T *>(::operator new(bytes));
    }

    void
    deallocate(T *const p, std::size_t const n) noexcept {
        std::size_t const bytes = n * sizeof(T);
        if (bytes >= region_cache::min_bytes) {
            region_cache::release(p, bytes);
            return;
        }
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            ::operator delete(p, std::align_val_t{alignof(T)}); // unsized: see the class note
        else
            ::operator delete(p); // unsized: see the class note
    }
};

/// Every `region_allocator` can free what any other allocated.
template <typename T, typename U>
[[nodiscard]] constexpr bool
operator==(region_allocator<T> const &, region_allocator<U> const &) noexcept {
    return true;
}

/// A `std::vector` whose storage, once it reaches `region_cache::min_bytes`, is a region.
template <typename T>
using region_vector = std::vector<T, region_allocator<T>>;

/// The deleter of `region_ptr`: destroys the object, then gives its storage back.
template <typename T>
struct region_delete {
    void
    operator()(T *const p) const noexcept {
        p->~T();
        region_allocator<T>{}.deallocate(p, 1);
    }
};

/// A `std::unique_ptr` to one object whose storage comes from `region_allocator`.
template <typename T>
using region_ptr = std::unique_ptr<T, region_delete<T>>;

/// `std::make_unique` for a `region_ptr`: the object is constructed from `args` (value-initialised
/// when there are none).
template <typename T, typename... Args>
[[nodiscard]] region_ptr<T>
make_region(Args &&...args) {
    T *const storage = region_allocator<T>{}.allocate(1);
    try {
        return region_ptr<T>(::new (static_cast<void *>(storage)) T(std::forward<Args>(args)...));
    } catch (...) {
        region_allocator<T>{}.deallocate(storage, 1);
        throw;
    }
}

} // namespace qb::allocator

#endif // QB_ALLOCATOR_REGION_H
