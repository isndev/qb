/**
 * @file qb/io/slab.cpp
 * @brief The platform half of `qb::allocator::slab_cache` — see the header for the why.
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

#include <qb/system/allocator/slab.h>
#include <qb/system/cpu.h> // spin_loop_pause

#include <atomic>
#include <cstdint>
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

} // namespace qb::allocator
