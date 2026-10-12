/**
 * @file qb/system/allocator/region.h
 * @brief Process-wide cache of page-granular memory regions: the per-core buffers one engine
 *        leaves behind, mapped and warm for the next one
 *
 * `region_cache` is where the buffers an engine builds per core, and whose size grows with the
 * engine, take their memory: the router's handler tables (one per event type, indexed by actor
 * id), the actor registry, the kill queues, the callback list, the broadcast snapshots, the receive
 * buffer and the mailboxes' blocks. It exists because of one regression and the margin its fix relied
 * on (Huly QB-1009, QB-1012). On glibc those buffers lived in `malloc`, and `malloc` decides on its
 * own whether a freed block goes back to the OS:
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
 * buffers no longer go through `malloc` at all. The policy, in full:
 *
 *  - **a region is a platform mapping** -- `mmap` on POSIX, `VirtualAlloc` reserve+commit on
 *    Windows -- of the request rounded up to a 4 KiB page. It is faulted on first touch, like the
 *    `malloc` block it replaces, and never touched by the cache while it is lent;
 *  - **a buffer a container outgrows is unmapped at once.** The containers above grow through
 *    `detail::region_reserve` / `region_resize` / `region_stack`, and the buffer a growth leaves
 *    behind -- a rung of the doubling ladder -- goes back to the OS on the spot: a running engine
 *    holds what it uses and nothing more;
 *  - **a buffer a container leaves when it dies is cached**, still mapped, on a process-wide list.
 *    The final buffers of an engine that stops stay warm for the next engine;
 *  - **a container that grows takes the largest cached region it can use** (between what it needs
 *    and its own ceiling, `detail::region_capacity`) and reserves that region's whole capacity,
 *    so the next engine's tables, registry and queues grow straight into the previous engine's
 *    final buffers in one step instead of climbing the ladder again. A plain request -- the
 *    receive buffer, a mailbox's block -- takes the most recently released region of its exact size;
 *  - **the retention is bounded**: the cache never holds more than twice the most bytes the
 *    process ever had lent at once, and past that bound the regions released longest ago are
 *    unmapped first; in practice it holds the final buffers of the engines that stopped.
 *    Every lookup -- an exact size, the largest within a range -- bisects a table of the sizes
 *    it holds, so its cost does not grow with the number of regions cached (on Windows the
 *    runtime-producer `mpsc::ringbuffer` leaves a region per ring there, Huly QB-203).
 *    `region_cache::trim()` unmaps everything cached: the call for a process that lives on after
 *    its engines and wants its footprint back, which nothing makes for it (the same contract as
 *    `slab_cache::trim()`, `slab.h`);
 *  - **a region can be RESERVED** (`detail::region_acquire_reserved`, Huly QB-203) for a block
 *    most of which its owner may never write -- an engine's mailbox, a ring per core of which
 *    only the header is written until that core sends. On Windows, where a committed mapping
 *    commits every page of it, it is `VirtualAlloc(MEM_RESERVE)` with its first page committed,
 *    its owner committing the rest as it needs it (`detail::region_commit`); it is cached with
 *    what was committed in it, apart from the committed regions of its size, and neither kind is
 *    ever lent for the other. On POSIX a mapping's pages are the process's once written, and a
 *    reserved region is a region like any other.
 *
 * `region_allocator<T>` is the standard-library face of it: a request of at least 64 KiB is a
 * region, a smaller one goes to the global allocator -- below 64 KiB a block neither reaches
 * glibc's thresholds by itself nor is worth a mapping, and 64 KiB is Windows' allocation
 * granularity. Everything else lives in `detail`: the cache is cold-path only -- a container
 * reaches it when it grows or dies, never per event -- and its lock blocks rather than spins (the
 * reason is `slab.cpp`'s). Its state is trivially destructible, so a container destroyed during
 * static destruction, or by a thread-local destructor at thread exit, still finds it. Never throws
 * except `std::bad_alloc` when the platform refuses a mapping.
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
 * @brief The process-wide cache of page-granular regions the engine's per-core buffers live in
 *        (the file comment says what it keeps and when). Its one public operation gives the
 *        cached regions back to the OS; everything else is `detail`.
 */
class region_cache {
public:
    /**
     * @brief Return every cached (released, unlent) region to the OS, and reset the high water to
     *        what is lent now. Thread-safe; a lent region is never touched.
     * @return The number of regions unmapped.
     */
    static std::size_t trim() noexcept;
};

namespace detail {

/// The smallest request `region_allocator` makes a region; smaller ones go to the global allocator.
inline constexpr std::size_t region_min_bytes = 64u * 1024u;
/// The unit a region's size is rounded up to: two requests that round alike share regions.
inline constexpr std::size_t region_page_bytes = 4096u;

/// `bytes` rounded up to `region_page_bytes` (at least one page): the size a region really has.
[[nodiscard]] constexpr std::size_t
region_rounded(std::size_t const bytes) noexcept {
    return bytes <= region_page_bytes ? region_page_bytes : (bytes + region_page_bytes - 1) / region_page_bytes * region_page_bytes;
}

/// A region of at least `bytes` bytes: the most recently released one of the same rounded size,
/// else a fresh mapping. Page-aligned; content unspecified. Throws `std::bad_alloc`.
[[nodiscard]] void *region_acquire(std::size_t bytes);
/// Give a region back: cached, unless the calling thread is inside a growth (`region_outgrow`),
/// which unmaps it. Null is ignored.
void region_release(void *region, std::size_t bytes) noexcept;
/// The size of the largest cached region within [`at_least`, `at_most`] bytes, 0 when none. A reserved region
/// (`region_acquire_reserved`) is never one.
[[nodiscard]] std::size_t region_largest_cached(std::size_t at_least, std::size_t at_most) noexcept;

/// A RESERVED region of at least `bytes` bytes (Huly QB-203): its address range is the caller's, its pages are not
/// -- only the first is committed, the rest is committed by `region_commit` as the caller needs it. The most recently
/// released reserved region of the same rounded size, whose pages keep what was committed in it, else a fresh
/// reservation. Windows: `VirtualAlloc(MEM_RESERVE)`; a committed region of the same size is never lent for it, nor it
/// for one. POSIX: a mapping's pages already are the process's only when written, so this is `region_acquire`.
/// Page-aligned; content unspecified. Throws `std::bad_alloc`.
[[nodiscard]] void *region_acquire_reserved(std::size_t bytes);
/// Commits the pages of [`at`, `at + bytes`) inside a reserved region, rounded out to whole pages; committed pages
/// are left as they are. False when the platform refuses (Windows: the commit limit). POSIX: true, nothing to do.
[[nodiscard]] bool region_commit(void *at, std::size_t bytes) noexcept;
/// Gives a reserved region back, with whatever is committed in it: cached among the reserved regions, unless the
/// calling thread is inside a growth (`region_outgrow`). Null is ignored. POSIX: `region_release`.
void region_release_reserved(void *region, std::size_t bytes) noexcept;

/// While one lives on a thread, a region that thread releases is unmapped instead of cached: the
/// growth helpers below hold one around the reallocation, so a rung a container outgrew never
/// reaches the cache.
class region_outgrow {
public:
    region_outgrow() noexcept;
    ~region_outgrow() noexcept;
    region_outgrow(region_outgrow const &)            = delete;
    region_outgrow &operator=(region_outgrow const &) = delete;

private:
    bool _previous;
};

/// The cache's counters, for tests and diagnostics.
struct region_stats_t {
    std::size_t cached;       ///< regions on the free list
    std::size_t cached_bytes; ///< their bytes
    std::size_t lent_bytes;   ///< bytes of the regions lent now
    std::size_t peak_bytes;   ///< the most bytes ever lent at once since the last trim(): half the bound
    std::size_t mappings;     ///< regions ever obtained from the platform (a cache hit does not count)
};
[[nodiscard]] region_stats_t region_stats() noexcept;

/**
 * @brief The capacity a growing container reserves: `want` elements, or -- when a cached region
 *        of at least `want` and at most `max_elements` elements exists -- that region's whole
 *        capacity, so it grows straight into a previous engine's final buffer.
 * @details A region's size is a page multiple and `T` is smaller than a page, so the capacity
 *          returned allocates exactly that region's rounded size: `region_acquire()` finds it.
 */
template <typename T>
[[nodiscard]] std::size_t
region_capacity(std::size_t const want, std::size_t const max_elements) noexcept {
    static_assert(sizeof(T) < region_page_bytes, "an element must be smaller than a page");
    if (want >= max_elements || want * sizeof(T) < region_min_bytes)
        return want;
    std::size_t const cached = region_largest_cached(region_rounded(want * sizeof(T)), max_elements * sizeof(T));
    return cached / sizeof(T) > want ? cached / sizeof(T) : want;
}

} // namespace detail

/**
 * @class region_allocator
 * @brief A stateless standard allocator: requests of at least 64 KiB are `region_cache` regions,
 *        smaller ones the global allocator's.
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
        static_assert(alignof(T) <= detail::region_page_bytes, "a region is page-aligned, no more");
        if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T))
            throw std::bad_array_new_length();
        std::size_t const bytes = n * sizeof(T);
        if (bytes >= detail::region_min_bytes)
            return static_cast<T *>(detail::region_acquire(bytes));
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return static_cast<T *>(::operator new(bytes, std::align_val_t{alignof(T)}));
        else
            return static_cast<T *>(::operator new(bytes));
    }

    void
    deallocate(T *const p, std::size_t const n) noexcept {
        std::size_t const bytes = n * sizeof(T);
        if (bytes >= detail::region_min_bytes) {
            detail::region_release(p, bytes);
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

/// A `std::vector` whose storage, once it reaches 64 KiB, is a region.
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

namespace detail {

/**
 * @brief Grow `v` to hold at least `need` elements: doubling, or straight into the largest cached
 *        region of at most `max_elements` elements; the buffer it leaves is unmapped, not cached.
 */
template <typename T>
void
region_reserve(region_vector<T> &v, std::size_t const need, std::size_t const max_elements) {
    if (need <= v.capacity())
        return;
    std::size_t const    doubled = v.capacity() * 2;
    region_outgrow const scope;
    v.reserve(region_capacity<T>(need > doubled ? need : doubled, max_elements));
}

/// `v.resize(n)` for a container that grows through `region_reserve` (`n` is clamped to `max_elements`).
template <typename T>
void
region_resize(region_vector<T> &v, std::size_t const n, std::size_t const max_elements) {
    std::size_t const size = n < max_elements ? n : max_elements;
    region_reserve(v, size, max_elements);
    v.resize(size);
}

/**
 * @class region_stack
 * @brief A `region_vector` whose `push_back` grows through `region_reserve`, for the containers
 *        that only ever grow one element at a time (a kill queue, a broadcast snapshot). Its
 *        ceiling is one core's id space (65 536 elements) or four times its capacity, whichever is
 *        larger. Everything else is the vector's.
 */
template <typename T>
class region_stack : public region_vector<T> {
public:
    using std::vector<T, region_allocator<T>>::vector;

    void
    push_back(T const &value) {
        if (this->size() == this->capacity()) [[unlikely]]
            region_reserve(*this, this->size() + 1, this->capacity() * 4 > 65536 ? this->capacity() * 4 : std::size_t{65536});
        region_vector<T>::push_back(value);
    }
};

} // namespace detail

} // namespace qb::allocator

#endif // QB_ALLOCATOR_REGION_H
