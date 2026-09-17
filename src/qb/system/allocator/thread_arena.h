/**
 * @file qb/system/allocator/thread_arena.h
 * @brief Per-thread size-class arena for small, thread-affine objects: the actor object's home.
 *
 * `thread_arena` is what `qb::Actor::operator new` / `operator delete` draw from. It exists
 * because of one measurement (qb-vs-others `savina/fib`, 57 312 actor lifetimes inside one
 * window, Huly QB-212): after 3.2's registry work the actor object's own `new` / `delete` was
 * the LAST heap traffic on an actor's lifetime -- exactly one `malloc` per actor, counted -- and
 * on WSL2 g++-14 that pair was **27.8 % of the core** (`perf`: 13.1 % `malloc`, 13.1 % inside
 * libc, 1.5 % `free`) of a 124 ns lifetime; on Windows/MSVC, whose heap is slower still, the
 * same lifetime read 179 ns. An actor is created and destroyed on ONE thread -- its core's --
 * by construction (`Actor::Actor()` asserts it, `VirtualCore::removeActor()` runs there), so a
 * thread-private allocator needs no lock, no atomics and no cross-thread free path at all.
 *
 * The design, and why each piece:
 *  - **size classes of 16 bytes up to `max_small`** (`granule`, `classes`): a block is rounded
 *    up to its class and returned to that class's free list, so a burst of one actor type
 *    recycles its own blocks in LIFO order -- the block a dying actor gives back is the one the
 *    next spawn takes, still in cache. Larger objects fall through to the global allocator and
 *    back through its unsized delete.
 *  - **bump allocation from chunks when a free list is empty**, never one `malloc` per block:
 *    the first chunk is 64 KiB from `::operator new` (a small engine's resting footprint stays
 *    small), every chunk after it is a 2 MB slab from `slab_cache` -- huge-page-backed and
 *    prefaulted on Linux, kept mapped and warm process-wide when the thread gives it back, so
 *    the second engine a process starts (or the next repetition of a benchmark) takes memory
 *    that is already faulted. That is the segmented pipe's lesson (`segmented_pipe.h`) applied
 *    to the actor object. Every chunk keeps its first `granule` bytes as a link word.
 *  - **constant-initialised thread-local state, touched by the hot path without a guard.** A
 *    `thread_local` with a destructor is reached through the TLS init wrapper (`__tls_init` on
 *    gcc: a guard check on every access, measured on the ask registry, Huly QB-178). The state
 *    here is a trivially constructible aggregate declared `constinit`, so `allocate()` and
 *    `deallocate()` are a plain TLS load away; the cleanup lives in a SEPARATE function-local
 *    thread-local (`reaper`), constructed once on the slow path that takes the first chunk and
 *    destroyed at thread exit -- the pattern `CoroutineFrameAllocator` uses, minus the guard.
 *  - **teardown gives the chunks back only when nothing is live.** At thread exit every actor
 *    of a `VirtualCore` is already destroyed (`VirtualCore` is a stack object of the worker
 *    thread, torn down before its thread-locals), so `live()` is 0 and the slabs go back to the
 *    cache. If it is not -- a block outliving its thread is a contract violation -- the chunks
 *    are ORPHANED rather than released: moved to a process-wide list where they stay mapped,
 *    reachable and counted (`orphaned()`), never recycled. A bounded, visible retention is the
 *    safe failure; a use-after-free through a recycled slab is not, and an unreachable chunk
 *    would read as a leak to a checker. A `deallocate()` that arrives after teardown is a no-op
 *    for the same reason.
 *
 * Single-threaded by contract: a block is freed on the thread that allocated it. Nothing here
 * checks it (the check would cost the thing this file exists to remove); `qb::Actor` enforces it
 * one level up. Never throws except `std::bad_alloc` from a refill the platform refuses.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - C++ Actor Framework (cpp.actor)
 * @ingroup Container
 */

#ifndef QB_ALLOCATOR_THREAD_ARENA_H
#define QB_ALLOCATOR_THREAD_ARENA_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <qb/system/allocator/slab.h>
#include <qb/utility/abi.h> /* QB_ABI_ANCHOR */

namespace qb::allocator {

/**
 * @class thread_arena
 * @brief Thread-private size-class allocator over `slab_cache` slabs (all members static; one
 *        arena per thread, reached through thread-local state).
 */
class thread_arena {
public:
    /// Size-class granularity and alignment of every block the arena hands out.
    static constexpr std::size_t granule = 16;
    /// Largest size the arena pools; anything larger goes to `::operator new` (unsized delete).
    static constexpr std::size_t max_small = 1024;
    /// Number of size classes: `[granule, max_small]` in `granule` steps.
    static constexpr std::size_t classes = max_small / granule;
    /// The first chunk, from `::operator new`: what an engine that creates a handful of actors
    /// holds per core. Every later chunk is a `slab_cache` slab.
    static constexpr std::size_t first_chunk_bytes = 64u * 1024u;
    static_assert(granule >= sizeof(void *), "a free block must hold its free-list link");
    static_assert(first_chunk_bytes > max_small + granule, "the first chunk must hold the largest block");
    static_assert(slab_cache::slab_bytes > max_small + granule, "a slab must hold the largest block");

    /**
     * @brief A block of at least `size` bytes, `granule`-aligned.
     * @details Pooled when `size <= max_small`; otherwise `::operator new(size)` -- pair it with
     *          `deallocate(p, size)` either way. May throw `std::bad_alloc`. The fall-through is
     *          given back through the UNSIZED global delete: the sized one is absent under
     *          `-fno-sized-deallocation` (an axis qb's ABI fingerprint builds), and the size
     *          buys nothing here.
     */
    [[nodiscard]] static void *
    allocate(std::size_t const size) {
        if (size > max_small)
            return ::operator new(size);
        const std::size_t idx = class_of(size);
        state            &st  = state_();
        if (void *const p = st.heads[idx]) {
            st.heads[idx] = *static_cast<void **>(p);
            ++st.live;
            return p;
        }
        const std::size_t bytes = (idx + 1) * granule;
        if (static_cast<std::size_t>(st.end - st.bump) < bytes)
            refill(st);
        void *const p = st.bump;
        st.bump += bytes;
        ++st.live;
        return p;
    }

    /**
     * @brief Give a block back. `size` is the size it was allocated with (the sized delete
     *        contract); a null `p` is ignored, and a block freed after the thread's arena was
     *        torn down is dropped (its chunk is released or orphaned, see the file comment).
     */
    static void
    deallocate(void *const p, std::size_t const size) noexcept {
        if (!p)
            return;
        if (size > max_small) {
            ::operator delete(p); // unsized: see allocate()
            return;
        }
        state &st = state_();
        if (st.reaped)
            return;
        const std::size_t idx    = class_of(size);
        *static_cast<void **>(p) = st.heads[idx];
        st.heads[idx]            = p;
        --st.live;
    }

    /**
     * @brief The over-aligned forms: an alignment above `granule` is served by the global
     *        aligned allocator (an actor type with `alignas(64)` is rare and stays correct).
     */
    [[nodiscard]] static void *
    allocate(std::size_t const size, std::size_t const align) {
        if (align <= granule)
            return allocate(size);
        return ::operator new(size, std::align_val_t{align});
    }
    static void
    deallocate(void *const p, std::size_t const size, std::size_t const align) noexcept {
        if (align <= granule) {
            deallocate(p, size);
            return;
        }
        ::operator delete(p, std::align_val_t{align}); // unsized: see allocate()
    }

    /// Blocks this thread has allocated (pooled classes) and not yet given back.
    [[nodiscard]] static std::size_t
    live() noexcept {
        return state_().live;
    }
    /// Chunks this thread holds: the first (64 KiB) chunk, then one per slab.
    [[nodiscard]] static std::size_t
    chunks() noexcept {
        return state_().chunk_count;
    }
    /// Bytes of chunk memory this thread holds (first chunk + slabs).
    [[nodiscard]] static std::size_t
    chunk_bytes() noexcept {
        const state &st = state_();
        return (st.first_chunk ? first_chunk_bytes : 0) + st.slab_count * slab_cache::slab_bytes;
    }
    /// Chunks of every thread that exited with a block still live: kept mapped and reachable on
    /// a process-wide list, never recycled (the file comment says why). Diagnostics only.
    [[nodiscard]] static std::size_t
    orphaned() noexcept {
        std::size_t n = 0;
        for (void *c = orphans_.load(std::memory_order_acquire); c; c = *static_cast<void **>(c))
            ++n;
        return n;
    }

private:
    /// Trivially constructible and value-initialised to all zeros (`st_{}` below), so the
    /// thread-local is constant-initialised: no TLS init guard on the hot path. Deliberately
    /// WITHOUT default member initializers: g++ treats a nested class's initializers as a
    /// complete-class context of the ENCLOSING class too, and refuses `st_{}` inside
    /// `thread_arena`'s body when they exist ("required before the end of its enclosing
    /// class"); zero is the right initial value of every member anyway. A chunk's first
    /// `granule` bytes hold its link to the next.
    struct state {
        void       *heads[classes]; ///< per-class free lists, null when empty
        char       *bump;           ///< next byte of the current chunk
        char       *end;            ///< one past the current chunk
        void       *first_chunk;    ///< the 64 KiB chunk from `::operator new`
        void       *slabs;          ///< singly linked through each slab's first word
        std::size_t live;           ///< pooled blocks handed out and not yet given back
        std::size_t chunk_count;
        std::size_t slab_count;
        bool        armed;  ///< the reaper thread-local exists
        bool        reaped; ///< teardown ran: chunks released or orphaned
    };

    [[nodiscard]] static constexpr std::size_t
    class_of(std::size_t const size) noexcept {
        return size == 0 ? 0 : (size + granule - 1) / granule - 1;
    }

    QB_ABI_ANCHOR static inline constinit thread_local state st_{};
    /// The orphan list: a lock-free stack of chunks linked through their first word.
    QB_ABI_ANCHOR static inline constinit std::atomic<void *> orphans_{nullptr};

    [[nodiscard]] static state &
    state_() noexcept {
        return st_;
    }

    /// Runs at thread exit, after every `VirtualCore` of the thread is gone.
    struct reaper {
        ~reaper() noexcept {
            teardown(st_);
        }
    };
    QB_ABI_ANCHOR static void
    arm_reaper() noexcept {
        thread_local reaper r;
        (void) &r;
    }

    static void
    refill(state &st) {
        if (!st.armed) {
            arm_reaper();
            st.armed = true;
        }
        char       *chunk;
        std::size_t bytes;
        if (!st.first_chunk) {
            chunk                             = static_cast<char *>(::operator new(first_chunk_bytes));
            bytes                             = first_chunk_bytes;
            *reinterpret_cast<void **>(chunk) = nullptr;
            st.first_chunk                    = chunk;
        } else {
            // throws bad_alloc when the platform refuses the mapping
            chunk                             = static_cast<char *>(slab_cache::acquire());
            bytes                             = slab_cache::slab_bytes;
            *reinterpret_cast<void **>(chunk) = st.slabs;
            st.slabs                          = chunk;
            ++st.slab_count;
        }
        st.bump = chunk + granule; // the link word stays untouched by blocks
        st.end  = chunk + bytes;
        ++st.chunk_count;
    }

    static void
    teardown(state &st) noexcept {
        st.reaped = true;
        for (auto &head : st.heads)
            head = nullptr;
        st.bump = st.end = nullptr;
        if (st.live != 0) {
            orphan(st); // a block outlives its thread: keep the chunks, never recycle them
        } else {
            for (void *slab = st.slabs; slab;) {
                void *const next = *static_cast<void **>(slab);
                slab_cache::release(slab);
                slab = next;
            }
            if (st.first_chunk)
                ::operator delete(st.first_chunk); // unsized: see allocate()
        }
        st.slabs       = nullptr;
        st.first_chunk = nullptr;
        st.slab_count  = 0;
        st.chunk_count = 0;
    }

    /// Push this thread's chunks -- the first chunk, then its slab chain -- onto the orphan list.
    static void
    orphan(state &st) noexcept {
        void *head = st.first_chunk;
        if (!head)
            return;                             // nothing was ever taken
        *static_cast<void **>(head) = st.slabs; // first chunk -> slab_k -> ... -> slab_1 -> null
        void *tail                  = head;
        while (void *const next = *static_cast<void **>(tail))
            tail = next;
        void *old = orphans_.load(std::memory_order_relaxed);
        do {
            *static_cast<void **>(tail) = old;
        } while (!orphans_.compare_exchange_weak(old, head, std::memory_order_release, std::memory_order_relaxed));
    }
};

} // namespace qb::allocator

#endif // QB_ALLOCATOR_THREAD_ARENA_H
