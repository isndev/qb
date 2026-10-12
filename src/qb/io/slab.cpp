/**
 * @file qb/io/slab.cpp
 * @brief The platform half of `qb::allocator::slab_cache` and of `qb::allocator::region_cache` —
 *        see the two headers (`slab.h`, `region.h`) for the why.
 *
 * Three mappers, one per platform family, behind one `map_slab()` / `unmap_slab()` pair:
 *  - POSIX: `mmap` twice the slab, trim to a 2 MB boundary. Linux then advises huge pages and
 *    asks the kernel to populate the mapping writable in one pass (`MADV_POPULATE_WRITE`,
 *    5.14+) — with THP in `madvise` or `always` mode that is one huge-page fault for the slab;
 *    without it, 512 base pages zeroed in one syscall rather than 512 traps. Either advice
 *    failing is not an error: the mapping is valid either way, only slower to first-touch.
 *  - Windows: `VirtualAlloc` reserve+commit. Committed pages are demand-zero on first touch and
 *    there is no unprivileged batch-populate, so the cache's reuse across engines is what makes
 *    the second engine warm there.
 *
 * The free list is intrusive — a cached slab's first word holds the next link — and the state
 * is a handful of trivially destructible statics under one lock (`std::mutex` where that is
 * trivially destructible, a yielding spinlock where it is not), taken only on cold paths.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - C++ Actor Framework (cpp.actor)
 * @ingroup Container
 */

#include <qb/system/allocator/region.h>
#include <qb/system/allocator/slab.h>
#include <qb/system/cpu.h> // spin_loop_pause

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>

#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__linux__) && !defined(MADV_POPULATE_WRITE)
#define MADV_POPULATE_WRITE 23 // linux 5.14; older headers with a newer kernel still get it
#endif
#endif

namespace qb::allocator {

namespace {

struct slab_link {
    slab_link *next;
};

// One cache per process. Every member is trivially destructible on purpose: a segment_pool torn
// down during static destruction (an engine held in a static, a test fixture's last breath) must
// still find a live cache to release into. That rules the lock's type too. `std::mutex` is the
// right primitive for a cold path every core of a starting engine reaches at once — a blocked
// waiter SLEEPS — and it is trivially destructible where the standard library builds it on a
// native lock that needs no teardown (libstdc++ on glibc, MSVC's SRWLOCK), so it is used there.
// Where it is not (libc++), the fallback is a test-and-test-and-set spinlock that yields after a
// short spin, never a pure spin: that is the start-barrier lesson again
// (`Main::__wait__all__cores__ready()`) — under ThreadSanitizer every atomic op takes the
// runtime's atomics mutex in READ mode, so 23 spinning `exchange`s starve the holder's releasing
// `store`, which needs it in WRITE mode, for ever. Measured: with a pure spin here,
// `MainLifecycle.StopMultiCoreGracefulNoError` at 24 cores hung past 600 s under TSan.
class yielding_spin_lock {
public:
    void
    lock() noexcept {
        for (unsigned spins = 0;; ++spins) {
            if (!_held.load(std::memory_order_relaxed) && !_held.exchange(true, std::memory_order_acquire))
                return;
            if (spins < 64)
                spin_loop_pause();
            else
                std::this_thread::yield();
        }
    }
    void
    unlock() noexcept {
        _held.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> _held{false};
};

using lock_type = std::conditional_t<std::is_trivially_destructible_v<std::mutex>, std::mutex, yielding_spin_lock>;
static_assert(std::is_trivially_destructible_v<lock_type>);
using scoped_lock = std::lock_guard<lock_type>;

lock_type         g_lock;
slab_link        *g_free       = nullptr;
std::size_t       g_cached     = 0;
std::size_t       g_mapped     = 0;
std::size_t       g_mappings   = 0;
std::atomic<bool> g_prefaulted = false;

#if defined(_WIN32) || defined(_WIN64)

void *
map_slab() noexcept {
    // Committed at once: the pool that asked for a slab is about to write into it, and a
    // reserve-only region would just move the commit into a first-touch fault.
    return ::VirtualAlloc(nullptr, slab_cache::slab_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void
unmap_slab(void *const slab) noexcept {
    ::VirtualFree(slab, 0, MEM_RELEASE);
}

std::size_t
platform_alignment() noexcept {
    return 64u * 1024u; // dwAllocationGranularity on every Windows that runs this code
}

#else // POSIX

void *
map_slab() noexcept {
    constexpr std::size_t bytes = slab_cache::slab_bytes;
#if defined(MAP_ANONYMOUS)
    constexpr int anon = MAP_ANONYMOUS;
#else
    constexpr int anon = MAP_ANON;
#endif
    // Over-map by one slab so a 2 MB-aligned slab is guaranteed inside, then give the
    // misaligned head and the surplus tail back. Two extra syscalls on a path taken once per
    // 2 MB of high water, for a mapping the kernel can then back with one huge page.
    void *const raw = ::mmap(nullptr, bytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | anon, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;
    auto const        raw_addr = reinterpret_cast<std::uintptr_t>(raw);
    auto const        base     = (raw_addr + bytes - 1) & ~static_cast<std::uintptr_t>(bytes - 1);
    std::size_t const head     = static_cast<std::size_t>(base - raw_addr);
    std::size_t const tail     = bytes - head;
    if (head)
        ::munmap(raw, head);
    if (tail)
        ::munmap(reinterpret_cast<void *>(base + bytes), tail);
    void *const slab = reinterpret_cast<void *>(base);
#if defined(__linux__)
#if defined(MADV_HUGEPAGE)
    ::madvise(slab, bytes, MADV_HUGEPAGE); // advice; refused on a THP=never kernel, harmlessly
#endif
    // Populate writable in one pass: one huge-page fault when THP took the advice, otherwise
    // 512 base pages zeroed inside the syscall. EINVAL on a kernel before 5.14: first-touch then.
    g_prefaulted.store(::madvise(slab, bytes, MADV_POPULATE_WRITE) == 0, std::memory_order_relaxed);
#endif
    return slab;
}

void
unmap_slab(void *const slab) noexcept {
    ::munmap(slab, slab_cache::slab_bytes);
}

std::size_t
platform_alignment() noexcept {
    return slab_cache::slab_bytes;
}

#endif

// ---------------------------------------------------------------------------------------------
// region_cache (region.h). A region is a plain mapping of its rounded size, faulted on first
// touch: a container writes its storage as it grows, never all at once, so nothing is prefaulted
// or advised -- what makes the second engine warm is that the region is still mapped.
// ---------------------------------------------------------------------------------------------

#if defined(_WIN32) || defined(_WIN64)

void *
map_region(std::size_t const bytes) noexcept {
    return ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void
unmap_region(void *const region, [[maybe_unused]] std::size_t const bytes) noexcept {
    ::VirtualFree(region, 0, MEM_RELEASE);
}

#else // POSIX

void *
map_region(std::size_t const bytes) noexcept {
#if defined(MAP_ANONYMOUS)
    constexpr int anon = MAP_ANONYMOUS;
#else
    constexpr int anon = MAP_ANON;
#endif
    void *const region = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | anon, -1, 0);
    return region == MAP_FAILED ? nullptr : region;
}

void
unmap_region(void *const region, std::size_t const bytes) noexcept {
    ::munmap(region, bytes);
}

#endif

// A CACHED region carries its links in its first bytes; a lent one carries nothing. Two lists run
// through the cached regions, each from the most recently released to the one released longest
// ago: the AGE list holds them all, and the bound unmaps from its old end; a SIZE list holds the
// regions of one rounded size, and `region_acquire()` lends from its new end -- the region of that
// size released last, as when one list held everything. Their own lock: a region never waits on a slab.
struct region_link {
    region_link *newer;      ///< by age: the next region released after this one
    region_link *older;      ///< by age: the next region released before it
    region_link *same_newer; ///< the next region of its size released after it
    region_link *same_older; ///< the next region of its size released before it
    std::size_t  bytes;      ///< the rounded size
};

// One entry per size that has a region cached, by ascending size: the two ends of that size's list.
// A lookup bisects it -- O(log sizes) under the lock, however many regions are cached. Walking the
// age list instead, as the cache first did, visited every cached region, a page each: an engine
// whose cores all talked leaves a mailbox ring per pair of cores in the cache (Huly QB-203), 4 032
// at 64 cores, and every buffer of another size the next engine asked for walked past all of them.
// The table is a mapping of its own, doubled under the lock when full -- a page holds 170 sizes, so
// in practice that is the first release of a process -- not counted as a region, unmapped by trim().
struct region_class {
    std::size_t  bytes;
    region_link *newest; ///< what region_acquire() lends next
    region_link *oldest;
};

lock_type     r_lock;
region_link  *r_newest       = nullptr;
region_link  *r_oldest       = nullptr;
region_class *r_classes      = nullptr; ///< r_class_count entries, sorted, in a mapping of r_class_bytes
std::size_t   r_class_count  = 0;
std::size_t   r_class_bytes  = 0;
std::size_t   r_cached       = 0;
std::size_t   r_cached_bytes = 0;
std::size_t   r_lent_bytes   = 0;
std::size_t   r_peak_bytes   = 0; ///< the high water of r_lent_bytes since the last trim()
std::size_t   r_mappings     = 0;

// Inside a growth (detail::region_outgrow): what this thread releases is unmapped, not cached.
// Constant-initialised and trivially destructible, so it costs no TLS guard and outlives nothing.
constinit thread_local bool t_outgrowing = false;

// Under r_lock: the index of the first class of at least `bytes`, r_class_count when there is none.
std::size_t
r_class_from(std::size_t const bytes) noexcept {
    if (!r_class_count)
        return 0; // and no arithmetic on a table that is not mapped yet
    return static_cast<std::size_t>(
        std::lower_bound(r_classes, r_classes + r_class_count, bytes, [](region_class const &c, std::size_t const b) { return c.bytes < b; })
        - r_classes);
}

// Under r_lock: the index of the first class of more than `bytes`, r_class_count when there is none.
std::size_t
r_class_after(std::size_t const bytes) noexcept {
    if (!r_class_count)
        return 0;
    return static_cast<std::size_t>(
        std::upper_bound(r_classes, r_classes + r_class_count, bytes, [](std::size_t const b, region_class const &c) { return b < c.bytes; })
        - r_classes);
}

// Under r_lock: a class for `bytes` at index `at` (where r_class_from() put it), inserted empty when
// there was none. The table doubles into a new mapping when full and the one it replaces is handed
// back through `retired` / `retired_bytes`, for the caller to unmap outside the lock. Null when the
// platform refuses that mapping: the size cannot be filed.
region_class *
r_class_for(std::size_t const bytes, std::size_t const at, region_class *&retired, std::size_t &retired_bytes) noexcept {
    if (at < r_class_count && r_classes[at].bytes == bytes)
        return &r_classes[at];
    if ((r_class_count + 1) * sizeof(region_class) > r_class_bytes) {
        std::size_t const grown = r_class_bytes ? 2 * r_class_bytes : detail::region_page_bytes;
        auto *const       table = static_cast<region_class *>(map_region(grown));
        if (!table)
            return nullptr;
        if (r_class_count)
            std::memcpy(table, r_classes, r_class_count * sizeof(region_class));
        retired       = r_classes;
        retired_bytes = r_class_bytes;
        r_classes     = table;
        r_class_bytes = grown;
    }
    std::memmove(r_classes + at + 1, r_classes + at, (r_class_count - at) * sizeof(region_class));
    r_classes[at] = region_class{bytes, nullptr, nullptr};
    ++r_class_count;
    return &r_classes[at];
}

// Under r_lock: `link` leaves both lists, and its class when it was the last of its size.
void
r_forget(region_link *const link) noexcept {
    (link->newer ? link->newer->older : r_newest) = link->older;
    (link->older ? link->older->newer : r_oldest) = link->newer;

    std::size_t const at  = r_class_from(link->bytes);
    region_class     &cls = r_classes[at];

    (link->same_newer ? link->same_newer->same_older : cls.newest) = link->same_older;
    (link->same_older ? link->same_older->same_newer : cls.oldest) = link->same_newer;
    if (!cls.newest) {
        std::memmove(r_classes + at, r_classes + at + 1, (r_class_count - at - 1) * sizeof(region_class));
        --r_class_count;
    }
    --r_cached;
    r_cached_bytes -= link->bytes;
}

// Under r_lock.
void
r_lend(std::size_t const bytes) noexcept {
    r_lent_bytes += bytes;
    if (r_lent_bytes > r_peak_bytes)
        r_peak_bytes = r_lent_bytes;
}

} // namespace

void *
slab_cache::acquire() {
    {
        scoped_lock const guard(g_lock);
        if (slab_link *const link = g_free) {
            g_free = link->next;
            --g_cached;
            return link;
        }
    }
    // Miss: map outside the lock (a syscall under a spinlock would make every other pool's cold
    // path spin for its duration), account inside it.
    void *const slab = map_slab();
    if (!slab)
        throw std::bad_alloc();
    scoped_lock const guard(g_lock);
    ++g_mapped;
    ++g_mappings;
    return slab;
}

void
slab_cache::release(void *const slab) noexcept {
    auto *const       link = static_cast<slab_link *>(slab);
    scoped_lock const guard(g_lock);
    link->next = g_free;
    g_free     = link;
    ++g_cached;
}

std::size_t
slab_cache::trim() noexcept {
    // Detach the whole list under the lock, unmap it outside: `munmap` is the slow part.
    slab_link  *list;
    std::size_t count;
    {
        scoped_lock const guard(g_lock);
        list     = g_free;
        count    = g_cached;
        g_free   = nullptr;
        g_cached = 0;
        g_mapped -= count;
    }
    while (list) {
        slab_link *const next = list->next;
        unmap_slab(list);
        list = next;
    }
    return count;
}

std::size_t
slab_cache::cached() noexcept {
    scoped_lock const guard(g_lock);
    return g_cached;
}

std::size_t
slab_cache::mapped() noexcept {
    scoped_lock const guard(g_lock);
    return g_mapped;
}

std::size_t
slab_cache::mappings() noexcept {
    scoped_lock const guard(g_lock);
    return g_mappings;
}

std::size_t
slab_cache::alignment() noexcept {
    return platform_alignment();
}

bool
slab_cache::prefaulted() noexcept {
    return g_prefaulted.load(std::memory_order_relaxed);
}

std::size_t
region_cache::trim() noexcept {
    // Detach the whole list and the size table under the lock, unmap them outside, as
    // slab_cache::trim() does.
    region_link  *list;
    std::size_t   count;
    region_class *table;
    std::size_t   table_bytes;
    {
        scoped_lock const guard(r_lock);
        list           = r_newest;
        count          = r_cached;
        table          = r_classes;
        table_bytes    = r_class_bytes;
        r_newest       = nullptr;
        r_oldest       = nullptr;
        r_classes      = nullptr;
        r_class_count  = 0;
        r_class_bytes  = 0;
        r_cached       = 0;
        r_cached_bytes = 0;
        r_peak_bytes   = r_lent_bytes;
    }
    while (list) {
        region_link *const next = list->older;
        unmap_region(list, list->bytes);
        list = next;
    }
    if (table)
        unmap_region(table, table_bytes);
    return count;
}

namespace detail {

void *
region_acquire(std::size_t const bytes) {
    if (bytes > (std::numeric_limits<std::size_t>::max)() - region_page_bytes)
        throw std::bad_alloc();
    std::size_t const size = region_rounded(bytes);
    {
        scoped_lock const guard(r_lock);
        if (std::size_t const at = r_class_from(size); at < r_class_count && r_classes[at].bytes == size) {
            region_link *const link = r_classes[at].newest; // the region of that size released last
            r_forget(link);
            r_lend(size);
            return link;
        }
    }
    // Miss: map outside the lock, account inside it -- slab_cache::acquire()'s reason.
    void *const region = map_region(size);
    if (!region)
        throw std::bad_alloc();
    scoped_lock const guard(r_lock);
    ++r_mappings;
    r_lend(size);
    return region;
}

void
region_release(void *const region, std::size_t const bytes) noexcept {
    if (!region)
        return;
    std::size_t const size = region_rounded(bytes);
    if (t_outgrowing) {
        // A rung a growing container left behind: back to the OS now, never into the cache.
        {
            scoped_lock const guard(r_lock);
            r_lent_bytes -= size;
        }
        unmap_region(region, size);
        return;
    }
    auto *const   link          = static_cast<region_link *>(region);
    region_link  *evicted       = nullptr; // past the bound, unmapped outside the lock, chained through `older`
    region_class *retired       = nullptr; // the size table a doubling replaced, unmapped outside the lock
    std::size_t   retired_bytes = 0;
    link->bytes                 = size;
    {
        scoped_lock const guard(r_lock);
        r_lent_bytes -= size;
        region_class *const cls = r_class_for(size, r_class_from(size), retired, retired_bytes);
        if (!cls) {
            // No page for a size table (the platform refused one): a region that cannot be filed is
            // not cached -- back to the OS, like one past the bound.
            link->older = nullptr;
            evicted     = link;
        } else {
            // The newest of all...
            link->newer = nullptr;
            link->older = r_newest;
            if (r_newest)
                r_newest->newer = link;
            else
                r_oldest = link;
            r_newest = link;
            // ...and of its size.
            link->same_newer = nullptr;
            link->same_older = cls->newest;
            if (cls->newest)
                cls->newest->same_newer = link;
            else
                cls->oldest = link;
            cls->newest = link;
            ++r_cached;
            r_cached_bytes += size;
            // The bound (region.h): at most twice the high water. It never reaches `link` itself:
            // the high water is at least its size, so once every older region is gone it holds.
            while (r_oldest != link && r_cached_bytes > r_peak_bytes && r_cached_bytes - r_peak_bytes > r_peak_bytes) {
                region_link *const oldest = r_oldest;
                r_forget(oldest);
                oldest->older = evicted;
                evicted       = oldest;
            }
        }
    }
    while (evicted) {
        region_link *const next = evicted->older;
        unmap_region(evicted, evicted->bytes);
        evicted = next;
    }
    if (retired)
        unmap_region(retired, retired_bytes);
}

std::size_t
region_largest_cached(std::size_t const at_least, std::size_t const at_most) noexcept {
    scoped_lock const guard(r_lock);
    // The largest cached size not above `at_most` -- every class holds a region -- if it reaches `at_least`.
    std::size_t const above = r_class_after(at_most);
    return above > 0 && r_classes[above - 1].bytes >= at_least ? r_classes[above - 1].bytes : 0;
}

region_outgrow::region_outgrow() noexcept
    : _previous(t_outgrowing) {
    t_outgrowing = true;
}

region_outgrow::~region_outgrow() noexcept {
    t_outgrowing = _previous;
}

region_stats_t
region_stats() noexcept {
    scoped_lock const guard(r_lock);
    return {r_cached, r_cached_bytes, r_lent_bytes, r_peak_bytes, r_mappings};
}

} // namespace detail

} // namespace qb::allocator
