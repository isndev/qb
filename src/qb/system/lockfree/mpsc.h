/**
 * @file qb/system/lockfree/mpsc.h
 * @brief Multiple-Producer Single-Consumer lockfree queue
 *
 * This file provides lockfree queue implementations that allow multiple producer threads
 * to safely enqueue items while a single consumer thread dequeues them, all without using
 * locks. These data structures are optimized for high-throughput concurrent systems where
 * multiple threads need to communicate with a single processing thread.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * @ingroup LockFree
 */
#ifndef QB_LOCKFREE_MPSC_H
#define QB_LOCKFREE_MPSC_H
#include <cassert>
#include <concepts>
#include <cstdint>
#include <mutex>
#include <new>
#include <qb/system/allocator/region.h>
#include "spinlock.h"
#include "spsc.h"

namespace qb::lockfree::mpsc {

/**
 * @brief Multi-Producer Single-Consumer ring buffer with fixed number of producers
 *
 * A lock-free MPSC ring buffer with a compile-time fixed number of producers, each with its own SPSC ring: no contention between them.
 * Three ways in: a producer chosen at compile time, `enqueue_at<Index>(...)`, or at run time, `enqueue(index, ...)` -- both lock-free,
 * one thread per index -- or a round-robin producer, `enqueue(t)` / `enqueue<_All>(t, size)`, any thread, under that producer's lock.
 *
 * @tparam T The type of elements stored in the buffer
 * @tparam max_size The maximum capacity per producer buffer
 * @tparam nb_producer The number of producers (fixed at compile time)
 */
template <typename T, std::size_t max_size, size_t nb_producer = 0>
class ringbuffer : public nocopy {
    using size_t = std::size_t;

    /**
     * @brief Producer data structure containing a lock and dedicated ring buffer
     *
     * Each producer has its own SPSC ring buffer and a lock to protect it when needed.
     * Cache line padding is used to avoid false sharing between producers.
     */
    struct Producer {
        constexpr static const int    padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
        SpinLock                      lock;                     ///< Lock for this producer
        char                          padding1[padding_size]{}; ///< Padding to avoid false sharing
        spsc::ringbuffer<T, max_size> _ringbuffer;              ///< The producer's dedicated ring buffer
    };

    std::array<Producer, nb_producer> _producers; ///< Array of producer structures

public:
    /**
     * @brief Enqueue an item into the ring of the producer chosen at compile time (no lock: one thread per index)
     *
     * @tparam _Index The compile-time index of the producer; `enqueue(index, t)` is the runtime-index form
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    template <size_t _Index>
    bool
    enqueue_at(T const &t) {
        return _producers[_Index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items into the ring of the producer chosen at compile time (no lock: one thread per index)
     *
     * @tparam _Index The compile-time index of the producer
     * @tparam _All A `bool`, never an integer: true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <size_t _Index, std::same_as<bool> auto _All = true>
    size_t
    enqueue_at(T const *t, size_t const size) {
        return _producers[_Index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a runtime producer index
     *
     * @param index The runtime index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    bool
    enqueue(size_t const index, T const &t) {
        return _producers[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a runtime producer index
     *
     * @tparam _All A `bool`, never an integer (`enqueue<0>(index, t, size)` does not compile): true, all items or none; false, as many as fit
     * @param index The runtime index of the producer
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(size_t const index, T const *t, size_t const size) {
        return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a per-thread round-robin producer index
     *
     * This method automatically selects a producer using a thread-local counter,
     * providing even load balancing across producers.
     *
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    size_t
    enqueue(T const &t) {
        static thread_local size_t tl_index = 0;
        const size_t               index    = tl_index++ % nb_producer;
        std::lock_guard<SpinLock>  lock(_producers[index].lock);
        return _producers[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a round-robin producer index
     *
     * Picks a producer by a per-thread round-robin counter (one per overload instance, shared by every ring of this type), locks it.
     * `enqueue<false>(t, size)` names this form, the compile-time index being `enqueue_at` (Huly QB-980).
     *
     * @tparam _All A `bool`, never an integer (`enqueue<0>(t, size)` does not compile): true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        static thread_local size_t tl_index = 0;
        const size_t               index    = tl_index++ % nb_producer;
        std::lock_guard<SpinLock>  lock(_producers[index].lock);
        return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Dequeue multiple items from all producers into one output array
     *
     * This method tries to dequeue items from all producers' buffers until
     * either the requested number of items is dequeued or all buffers are empty.
     *
     * @param ret Output array receiving the dequeued items. Each producer **appends**
     *            after the previous one's items, so this must have room for @p size.
     * @param size **Total** budget across all producers — the return value never exceeds it.
     * @return The number of items successfully dequeued, in `[0, size]`.
     *
     * @warning `size` does **not** mean the same thing here as it does in the functor
     *          overload below, because @p ret does not play the same role. Here @p ret is
     *          the *output*: producers append into it, so the budget has to be global or the
     *          array would overflow. In `dequeue(Func const&, T*, size_t)` @p ret is a
     *          *scratch* buffer reused once per producer, so `size` is that buffer's capacity
     *          and therefore a **per-producer** chunk limit. The two are deliberately
     *          different and neither can adopt the other's meaning; see that overload's own
     *          warning for why the engine depends on the per-producer form.
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        for (auto &producer : _producers) {
            // Advance ret by the amount taken so each producer appends after
            // the previous one's items instead of overwriting them (silent
            // data loss: items consumed from the ring but lost in the output).
            const size_t n = producer._ringbuffer.dequeue(ret, size);
            ret += n;
            size -= n;
            if (!size)
                break;
        }
        return save_size - size;
    }

    /**
     * @brief Does any producer ring hold at least one unread item?
     * @details Consumer thread only, for a park/unpark decision: each ring's `empty()` reads the
     *          producer's published write index and the consumer's own private read index; no lock.
     *          Meant to be evaluated AFTER a `std::atomic_thread_fence(seq_cst)` by the consumer
     *          that has just announced it is about to block (the Dekker half of a race-free
     *          wait), so that a producer's enqueue is either seen here or sees the announcement.
     * @return true if at least one ring is non-empty.
     */
    [[nodiscard]] bool
    has_data() const noexcept { // consumer thread only: `empty()` reads the consumer's private index
        for (auto const &producer : _producers)
            if (!producer._ringbuffer.empty())
                return true;
        return false;
    }

    /**
     * @brief Process all available items from all producers, in place
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @return The number of items successfully processed
     *
     * @warning Walks each ring IN PLACE, so a readable range that wraps the end of the
     *          buffer invokes @p func twice on two disjoint segments — which splits any
     *          logical item spanning several slots. For those, use the copy-out overload
     *          below, which reassembles each batch contiguously.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func) {
        size_t nb_consume = 0;
        for (auto &producer : _producers) {
            nb_consume += producer._ringbuffer.consume_all(func);
        }
        return nb_consume;
    }

    /**
     * @brief Drain every producer through a scratch buffer, one batch per producer
     *
     * Calls `func(scratch, n)` once per non-empty producer ring, where `n` is the number of
     * items copied out of *that* ring. Unlike `consume_all(func)` above, each batch is
     * copied out contiguously first, so an item spanning several slots survives a wrap.
     *
     * @tparam Func Batch functor, invoked as `func(T *batch, std::size_t count)`
     * @param func Functor consuming one producer's batch; must consume it before returning,
     *             because the next producer reuses @p scratch
     * @param scratch Buffer of capacity @p chunk, rewritten from index 0 for every producer
     *                — not an accumulating output array
     * @param chunk Capacity of @p scratch, and hence the **per-producer** batch limit
     * @return Total items drained across all producers — **may exceed @p chunk**, up to
     *         `nb_producer * chunk`
     *
     * @note **This is `consume_all`, not `dequeue`, and the name is the contract.** It was
     *       spelled `dequeue(func, ret, size)` until 3.0, which made it look like a bounded
     *       sibling of `dequeue(T*, size)` when it is not: that one treats `size` as a total
     *       budget and returns at most `size`; this one treats it as a per-producer chunk and
     *       returns a total. Worse, one layer down the same name *is* bounded —
     *       `spsc::ringbuffer::dequeue(func, ret, size)` copies out at most `size` and calls
     *       the functor once — so it was the mpsc loop that silently changed the meaning of
     *       the argument it forwarded, and a reader who had learned the spsc primitive (which
     *       is exactly what `ringOf(i)` hands them) was misled by the layer above.
     *
     *       The per-producer budget is deliberate and load-bearing, which is why the fix was
     *       the name rather than the behaviour: `VirtualCore::__receive__` passes the scratch
     *       capacity (`MaxRingEvents`, one ring's readable maximum) precisely so that
     *       **every** core's ring is drained on every turn. Carry a budget across the loop
     *       and the first saturated producer consumes it while cores 2..N are never read —
     *       starvation on the engine's event path, under exactly the load that makes it
     *       matter. If you need a bounded total, call `dequeue(T*, size)`.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func, T *scratch, size_t const chunk) {
        size_t nb_consume = 0;
        for (auto &producer : _producers) {
            // `chunk` is deliberately NOT decremented across producers — see the note above.
            // `scratch` is rewritten from index 0 by each producer's copy-out.
            nb_consume += producer._ringbuffer.dequeue(func, scratch, chunk);
        }
        return nb_consume;
    }

    /**
     * @brief Get direct access to a specific producer's ring buffer
     *
     * @param index The index of the producer
     * @return Reference to the producer's ring buffer
     */
    auto &
    ringOf(size_t const index) {
        return _producers[index]._ringbuffer;
    }
};

/**
 * @brief Multi-Producer Single-Consumer ring buffer with runtime-determined number of
 * producers
 *
 * This specialization provides a MPSC ring buffer with a runtime-determined number of producers specified during construction.
 * Same three ways in as the fixed-producer form: `enqueue_at<Index>(...)`, `enqueue(index, ...)`, and the round-robin `enqueue(...)`.
 *
 * **A producer's ring costs nothing until it is constructed** (Huly QB-203). Construction allocates one pointer per
 * producer -- and on POSIX one block with room for every producer's ring, untouched -- and the first enqueue on an
 * index, or `ringOf(index)`, constructs that producer's ring and publishes it; the consumer's walks skip an index
 * whose ring was never published. Memory is therefore proportional to the producers that have produced, not to the
 * producers that might. Until 3.3 the rings were a `std::vector<Producer>` of `nb_producer` elements,
 * value-initialised: every byte of every ring -- a `max_size + 1` slot array each -- was written at construction.
 * The engine's mailbox, whose producers all produce, is not this type: it is `block_ringbuffer` below, every ring
 * built at once.
 *
 * - **Storage.** CONTIGUOUS (POSIX): one block per buffer from `qb::allocator::region_allocator` (`region.h`, Huly
 *   QB-1012) -- from 64 KiB a `region_cache` region -- with room for `nb_producer + 1` rings and, in its last bytes,
 *   the slot lines, taken by the constructor and left as the allocator hands it: a page nothing was written into is
 *   not the process's. Each ring is constructed in its place in the block; the buffer holds nothing on the heap but
 *   its own object. PER RING (Windows): each ring is a region of its own, allocated when the ring is constructed,
 *   because a committed mapping commits every page of a block, written or not; the slots are an allocation of their
 *   own. Destroyed, a region stays mapped in that cache, so the next buffer of its size is built in pages already
 *   faulted, until `region_cache::trim()` gives them back.
 * - **Placement.** A region is page-aligned, so rings placed at the start of theirs would put the same line of every
 *   ring -- the consumer's read index, the producer's write index -- at the same offset in a page: one L1 set for
 *   all of them, which a consumer walking more rings than the set has ways misses on every walk, and the same low
 *   twelve address bits, which the store buffer cannot tell apart until a store commits (4K aliasing -- what
 *   `segmented_pipe` staggers its segments against). A ring therefore starts `((5 * index + s) % slots) *
 *   alignof(Producer)` bytes into its region, `s` the buffer's colour -- the next value of a counter -- in the bytes
 *   the region's page rounding leaves anyway (PER RING); the block starts `s` slots in, and its stride, a ring and
 *   640 bytes past a page multiple for an engine-sized ring, moves each ring five slots on.
 * - **Publication.** A slot is CLAIMED -- a compare-exchange from null to a value no ring has -- before its ring is
 *   constructed, by default-initialisation (placement `new Producer`, never `Producer()`): the lock and the ring's
 *   four index blocks are written, the slot array is left untouched, pages the process does not hold yet. The ring
 *   is then published by a release store; every reader of a slot loads it with acquire, so what the constructor
 *   wrote happens-before any use of the ring on another thread, and a claimed slot reads as empty -- one unsigned
 *   comparison. One thread per index never contends; the round-robin forms and `ringOf` may race the index's owner,
 *   and the thread that finds the slot claimed waits the few stores the winner has left, then uses its ring.
 * - **What each path pays.** A producer: one load of its slot and a branch it predicts after the first enqueue; the
 *   construction itself is out of line. The consumer: one load per producer per walk -- a cache line holds eight
 *   slots with 64-byte lines, and the slot array is whole lines so nothing else shares them -- and an index with no
 *   ring costs that load and nothing more, where it used to cost the two lines of a ring's header. A consumer whose
 *   walks skip empty indices returns to its live rings sooner and polls their published indices harder: an engine
 *   whose mailboxes were this type ran a chain of eight cores passing one event 9 % slower (46 to 47 loop passes per
 *   round trip against 35 to 36), which is why the engine's mailbox is `block_ringbuffer`.
 * - **Failure.** On Windows the first enqueue on an index allocates, so it can throw `std::bad_alloc`; every later
 *   one cannot. On POSIX the constructor took the block, and no enqueue allocates.
 *
 * @tparam T The type of elements stored in the buffer
 * @tparam max_size The maximum capacity per producer buffer
 */
template <typename T, std::size_t max_size>
class ringbuffer<T, max_size, 0> : public nocopy {
    using size_t = std::size_t;

    /**
     * @brief Producer data structure containing a lock and dedicated ring buffer
     *
     * Each producer has its own SPSC ring buffer and a lock to protect it when needed.
     * Cache line padding is used to avoid false sharing between producers.
     */
    struct Producer {
        constexpr static const int    padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
        SpinLock                      lock;                     ///< Lock for this producer
        char                          padding1[padding_size]{}; ///< Padding to avoid false sharing
        spsc::ringbuffer<T, max_size> _ringbuffer;              ///< The producer's dedicated ring buffer
    };

    /// A producer's slot: its published ring, or null until its first enqueue.
    using Slot                             = std::atomic<Producer *>;
    static constexpr size_t slots_per_line = QB_LOCKFREE_CACHELINE_BYTES / sizeof(Slot);
    /// One cache line of slots. The array is made of whole lines, so no other object shares a line the consumer reads
    /// on every walk.
    struct alignas(QB_LOCKFREE_CACHELINE_BYTES) SlotLine {
        Slot entry[slots_per_line]{};
    };
    static_assert(sizeof(SlotLine) == QB_LOCKFREE_CACHELINE_BYTES, "a line of slots is one cache line, no padding");

    /// From 64 KiB a ring's storage is a region, rounded up to whole pages, and the ring starts at a staggered offset in it
    /// (the class note says why); below, it is a heap block and the ring starts at its beginning.
    static constexpr bool   ring_in_region = sizeof(Producer) >= qb::allocator::detail::region_min_bytes;
    static constexpr size_t ring_storage   = ring_in_region ? qb::allocator::detail::region_rounded(sizeof(Producer)) : sizeof(Producer);
    /// The offsets a ring can start at, `alignof(Producer)` apart, in the bytes the page rounding leaves (28 for the engine's).
    static constexpr size_t stagger_slots = (ring_storage - sizeof(Producer)) / alignof(Producer) + 1;
    static_assert(ring_storage - sizeof(Producer) < qb::allocator::detail::region_page_bytes,
                  "a stagger lives in what the page rounding leaves: it never costs a page");
    static_assert(stagger_slots * alignof(Producer) <= sizeof(Producer), "a block's stagger fits in its spare ring");

    /// Where the rings live. CONTIGUOUS (POSIX): one block holds every producer's ring, taken by the constructor -- the
    /// storage shape of the vector of rings this replaced, from 64 KiB a region of the cache -- and a producer's ring is
    /// constructed in place by its first enqueue: the pages of the block no ring was written into are not the process's.
    /// PER RING (Windows): each ring is a region of its own, allocated by its first enqueue, because a committed mapping
    /// commits every ring of a block, written or not (Huly QB-203).
#if defined(_WIN32) || defined(_WIN64)
    static constexpr bool contiguous = false;
#else
    static constexpr bool contiguous = true;
#endif

    Producer *const     _block;       ///< CONTIGUOUS: the rings' block, `block_rings` rings long, its slots at the end; else null
    SlotLine *const     _slots;       ///< One slot per producer, `slots_per_line` to a line: CONTIGUOUS, the block's last bytes
    const std::uint32_t _nb_producer; ///< Number of producers (32 bits: the base stays the 32 bytes the vector's was)
    const std::uint32_t _stagger;     ///< This buffer's colour, its first stagger slot: see `stagger_of`

    /// The slot lines of a buffer of @p nb producers.
    [[nodiscard]] static constexpr size_t
    lines_for(size_t const nb) noexcept {
        return (nb + slots_per_line - 1) / slots_per_line;
    }

    /// CONTIGUOUS: the block, in rings -- `nb + 1`, the last one the stagger's slack, and past them, only when that slack
    /// cannot hold them, the slot lines, which take the block's last bytes. Rings of the engine's size have room to spare:
    /// 2 KiB of lines at 256 cores against at least 62 720 bytes of slack.
    [[nodiscard]] static constexpr size_t
    block_rings(size_t const nb) noexcept {
        size_t const slot_bytes = lines_for(nb) * sizeof(SlotLine);
        size_t const slack      = sizeof(Producer) - (stagger_slots - 1) * alignof(Producer); // the least, past the last ring
        return nb + 1 + (slot_bytes <= slack ? 0 : (slot_bytes + sizeof(Producer) - 1) / sizeof(Producer));
    }

    /// The zeroed slot lines: CONTIGUOUS, in the block's last bytes -- no allocation of their own, so a buffer is its
    /// object and its block and nothing on the heap between two buffers; PER RING, an aligned allocation.
    [[nodiscard]] static SlotLine *
    make_slots([[maybe_unused]] Producer *const block, size_t const nb) {
        size_t const   lines = lines_for(nb);
        unsigned char *at    = nullptr;
        if constexpr (contiguous)
            at = reinterpret_cast<unsigned char *>(block) + block_rings(nb) * sizeof(Producer) - lines * sizeof(SlotLine);
        else
            at = static_cast<unsigned char *>(::operator new(lines * sizeof(SlotLine), std::align_val_t{alignof(SlotLine)}));
        for (size_t i = 0; i < lines; ++i)
            ::new (static_cast<void *>(at + i * sizeof(SlotLine))) SlotLine{};
        return std::launder(reinterpret_cast<SlotLine *>(at));
    }

    /// A buffer's colour: consecutive buffers take consecutive first slots.
    [[nodiscard]] static size_t
    next_stagger() noexcept {
        static constinit std::atomic<size_t> built{0};
        return built.fetch_add(1, std::memory_order_relaxed) % stagger_slots;
    }

    /// Where producer @p index's ring starts in its region (PER RING): five slots further for each producer and one
    /// for each step of the buffer's colour. CONTIGUOUS starts the block at the colour and each ring five slots (66 176
    /// bytes, sixteen pages and 640 bytes) past the one before it.
    [[nodiscard]] size_t
    stagger_of(size_t const index) const noexcept {
        return (index * 5 + _stagger) % stagger_slots * alignof(Producer);
    }

    /// The bytes producer @p index's ring is constructed in: its place in the block, or a region allocated now.
    [[nodiscard]] void *
    ring_storage_for(size_t const index) const {
        if constexpr (contiguous)
            return reinterpret_cast<unsigned char *>(_block) + _stagger * alignof(Producer) + index * sizeof(Producer);
        else if constexpr (ring_in_region)
            return qb::allocator::region_allocator<unsigned char>{}.allocate(ring_storage) + stagger_of(index);
        else
            return qb::allocator::region_allocator<Producer>{}.allocate(1);
    }

    [[nodiscard]] Slot &
    slot(size_t const index) const noexcept {
        return _slots[index / slots_per_line].entry[index % slots_per_line];
    }

    /// A slot's value while the thread that claimed it constructs its ring: never a ring's address.
    [[nodiscard]] static Producer *
    claimed() noexcept {
        return reinterpret_cast<Producer *>(std::uintptr_t{1});
    }

    /// A ring, as opposed to null (no ring yet) or `claimed()` (one being constructed): one unsigned comparison.
    [[nodiscard]] static bool
    usable(Producer const *const ring) noexcept {
        return reinterpret_cast<std::uintptr_t>(ring) > std::uintptr_t{1};
    }

    /// The ring producer @p index has published, or null.
    [[nodiscard]] Producer *
    published(size_t const index) const noexcept {
        Producer *const ring = slot(index).load(std::memory_order_acquire);
        return usable(ring) ? ring : nullptr;
    }

    /// What every consumer walk is: @p each(ring) for every published ring, in producer order, until it returns true.
    template <typename Each>
    bool
    walk(Each &&each) const {
        for (size_t i = 0; i < _nb_producer; ++i)
            if (Producer *const ring = published(i); ring && each(*ring))
                return true;
        return false;
    }

    /// The ring of producer @p index, constructed by the first call that needs it.
    [[nodiscard]] Producer &
    producer(size_t const index) {
        if (Producer *const ring = slot(index).load(std::memory_order_acquire); likely(usable(ring))) [[likely]]
            return *ring;
        return allocate_producer(index);
    }

    // The cold half of `producer()`, out of line so that an enqueue path inlining the hot half keeps its registers. The
    // slot is CLAIMED before the ring is constructed -- in the block, two threads would otherwise construct one ring at
    // one address -- and published once it is; a thread that finds it claimed waits the few stores that takes.
    [[nodiscard]] QB_NOINLINE QB_COLD Producer &
    allocate_producer(size_t const index) {
        Slot &s = slot(index);
        for (;;) {
            Producer *seen = s.load(std::memory_order_acquire);
            if (usable(seen))
                return *seen;
            if (seen == nullptr && s.compare_exchange_strong(seen, claimed(), std::memory_order_acquire, std::memory_order_acquire)) {
                void *storage;
                try {
                    storage = ring_storage_for(index);
                } catch (...) {
                    s.store(nullptr, std::memory_order_release); // unclaimed: the next caller tries again
                    throw;
                }
                // Default-initialisation -- placement `new Producer`, never `Producer()` and never `make_region`, both
                // of which value-initialise: the slot array stays untouched. See the class note.
                Producer *const fresh = ::new (storage) Producer;
                s.store(fresh, std::memory_order_release);
                return *fresh;
            }
            spin_loop_pause(); // claimed by another thread: its ring is a few stores away
        }
    }

    /// Destroys the ring `allocate_producer(index)` made and gives its storage back (PER RING: a region to the cache).
    void
    release([[maybe_unused]] size_t const index, Producer *const ring) const noexcept {
        ring->~Producer();
        if constexpr (contiguous)
            return; // the block goes with the buffer
        else if constexpr (ring_in_region)
            qb::allocator::region_allocator<unsigned char>{}.deallocate(reinterpret_cast<unsigned char *>(ring) - stagger_of(index),
                                                                        ring_storage);
        else
            qb::allocator::region_allocator<Producer>{}.deallocate(ring, 1);
    }

public:
    /**
     * @brief Default constructor is deleted - must specify number of producers
     */
    ringbuffer() = delete;

    /**
     * @brief Constructor with runtime number of producers
     *
     * @param nb_producer The number of producers to create
     * @details Allocates one slot per producer and no ring -- a producer's ring is constructed by its first enqueue --
     *          and, CONTIGUOUS, the block the rings will be constructed in, untouched but for the slots at its end.
     */
    explicit ringbuffer(std::size_t const nb_producer)
        : _block(contiguous ? qb::allocator::region_allocator<Producer>{}.allocate(block_rings(nb_producer)) : nullptr)
        , _slots(make_slots(_block, nb_producer))
        , _nb_producer(static_cast<std::uint32_t>(nb_producer))
        , _stagger(static_cast<std::uint32_t>(next_stagger())) {
        // At least one producer is required: the round-robin enqueue paths
        // compute `tl_index % _nb_producer`, which is division-by-zero (UB)
        // when constructed with zero producers.
        assert(nb_producer > 0 && "mpsc::ringbuffer requires at least one producer");
        assert(nb_producer <= UINT32_MAX && "mpsc::ringbuffer counts its producers in 32 bits");
    }

    /**
     * @brief Destroys every ring a producer constructed, and gives the storage back. No producer or consumer may still
     *        be using the buffer.
     */
    ~ringbuffer() noexcept {
        for (size_t i = 0; i < _nb_producer; ++i)
            if (Producer *const ring = slot(i).load(std::memory_order_relaxed); usable(ring))
                release(i, ring);
        if constexpr (contiguous)
            qb::allocator::region_allocator<Producer>{}.deallocate(_block, block_rings(_nb_producer)); // the slots with it
        else
            ::operator delete(static_cast<void *>(_slots), std::align_val_t{alignof(SlotLine)}); // trivially destructible lines
    }

    /**
     * @brief Enqueue an item into the ring of the producer chosen at compile time (no lock: one thread per index)
     *
     * @tparam _Index The compile-time index of the producer; `enqueue(index, t)` is the runtime-index form
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    template <size_t _Index>
    bool
    enqueue_at(T const &t) {
        return producer(_Index)._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items into the ring of the producer chosen at compile time (no lock: one thread per index)
     *
     * @tparam _Index The compile-time index of the producer
     * @tparam _All A `bool`, never an integer: true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <size_t _Index, std::same_as<bool> auto _All = true>
    size_t
    enqueue_at(T const *t, size_t const size) {
        return producer(_Index)._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a runtime producer index
     *
     * @param index The runtime index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    bool
    enqueue(size_t const index, T const &t) {
        return producer(index)._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a runtime producer index
     *
     * @tparam _All A `bool`, never an integer (`enqueue<0>(index, t, size)` does not compile): true, all items or none; false, as many as fit
     * @param index The runtime index of the producer
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(size_t const index, T const *t, size_t const size) {
        return producer(index)._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a per-thread round-robin producer index
     *
     * This method automatically selects a producer using a thread-local counter,
     * providing even load balancing across producers.
     *
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    size_t
    enqueue(T const &t) {
        static thread_local size_t tl_index = 0;
        Producer                  &chosen   = producer(tl_index++ % _nb_producer);
        std::lock_guard<SpinLock>  lock(chosen.lock);
        return chosen._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a round-robin producer index
     *
     * The producer is picked, then locked, by a counter per overload instance and shared by every ring of this type: a per-thread
     * round-robin counter, providing even load balancing across producers.
     *
     * @tparam _All A `bool`, never an integer (`enqueue<0>(t, size)` does not compile): true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        static thread_local size_t tl_index = 0;
        Producer                  &chosen   = producer(tl_index++ % _nb_producer);
        std::lock_guard<SpinLock>  lock(chosen.lock);
        return chosen._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Dequeue multiple items from all producers into one output array
     *
     * This method tries to dequeue items from all producers' buffers until
     * either the requested number of items is dequeued or all buffers are empty.
     *
     * @param ret Output array receiving the dequeued items. Each producer **appends**
     *            after the previous one's items, so this must have room for @p size.
     * @param size **Total** budget across all producers — the return value never exceeds it.
     * @return The number of items successfully dequeued, in `[0, size]`.
     *
     * @warning `size` does **not** mean the same thing here as it does in the functor
     *          overload below — see that overload's warning. Here @p ret is the *output* and
     *          the budget is global; there @p ret is *scratch* and `size` is a per-producer
     *          chunk limit.
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        walk([&ret, &size](Producer &ring) { // a producer that never enqueued has no ring: nothing to take
            // Advance ret by the amount taken so each producer appends after
            // the previous one's items instead of overwriting them (silent
            // data loss: items consumed from the ring but lost in the output).
            const size_t n = ring._ringbuffer.dequeue(ret, size);
            ret += n;
            size -= n;
            return size == 0;
        });
        return save_size - size;
    }

    /**
     * @brief Does any producer ring hold at least one unread item?
     * @details Consumer thread only, for a park/unpark decision: each ring's `empty()` reads the
     *          producer's published write index and the consumer's own private read index; no lock.
     *          A producer with no ring yet has enqueued nothing: its slot is read, its ring is not. A
     *          ring is published before its first item and so before the producer's fence: the
     *          handshake covers the slot exactly as it covers the write index.
     *          Meant to be evaluated AFTER a `std::atomic_thread_fence(seq_cst)` by the consumer
     *          that has just announced it is about to block (the Dekker half of a race-free
     *          wait), so that a producer's enqueue is either seen here or sees the announcement.
     * @return true if at least one ring is non-empty.
     */
    [[nodiscard]] bool
    has_data() const noexcept { // consumer thread only: `empty()` reads the consumer's private index
        return walk([](Producer const &ring) { return !ring._ringbuffer.empty(); });
    }

    /**
     * @brief Process all available items from all producers, in place
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @return The number of items successfully processed
     *
     * @warning Walks each ring IN PLACE, so a readable range that wraps the end of the
     *          buffer invokes @p func twice on two disjoint segments — which splits any
     *          logical item spanning several slots (a multi-bucket `qb::Event` is exactly
     *          that). For those, use the copy-out overload below.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func) {
        size_t nb_consume = 0;
        walk([&nb_consume, &func](Producer &ring) {
            nb_consume += ring._ringbuffer.consume_all(func);
            return false;
        });
        return nb_consume;
    }

    /**
     * @brief Drain every producer through a scratch buffer, one batch per producer
     *
     * Calls `func(scratch, n)` once per non-empty producer ring, where `n` is the number of
     * items copied out of *that* ring. Unlike `consume_all(func)` above, each batch is
     * copied out contiguously first, so an item spanning several slots survives a wrap —
     * which is why the engine drains its mailboxes with this overload's twin (`block_ringbuffer`).
     *
     * @tparam Func Batch functor, invoked as `func(T *batch, std::size_t count)`
     * @param func Functor consuming one producer's batch; must consume it before returning,
     *             because the next producer reuses @p scratch
     * @param scratch Buffer of capacity @p chunk, rewritten from index 0 for every producer
     *                — not an accumulating output array
     * @param chunk Capacity of @p scratch, and hence the **per-producer** batch limit
     * @return Total items drained across all producers — **may exceed @p chunk**, up to
     *         `_nb_producer * chunk`
     *
     * @note **This is `consume_all`, not `dequeue`, and the name is the contract.** See the
     *       fixed-size sibling above for the full account: it was spelled
     *       `dequeue(func, ret, size)` until 3.0, which made it read as a bounded sibling of
     *       `dequeue(T*, size)` when it is not — and the same name one layer down, on
     *       `spsc::ringbuffer`, genuinely *is* bounded. `VirtualCore::__receive__` and
     *       `SharedCoreCommunication`'s teardown sweep both depend on the twin's per-producer form:
     *       a budget shared across the loop would let one saturated producer consume it and
     *       starve every other core's ring. For a bounded total, call `dequeue(T*, size)`.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func, T *scratch, size_t const chunk) {
        size_t nb_consume = 0;
        walk([&nb_consume, &func, scratch, chunk](Producer &ring) { // a producer that never enqueued has no ring
            // The poll, inline (Huly QB-257): a ring whose published write index is where this
            // consumer's read index stands has nothing to copy out, and the copy-out below is a call
            // per ring on MSVC -- on every pass of every core, for rings that are empty on every
            // idle pass. `empty()` reads the published line `dequeue` reads first; a producer's
            // store this relaxed load does not see yet is seen by the next pass, exactly as the
            // acquire load inside `dequeue` would have seen it then (read-read coherence: the
            // acquire load never reads an older value than this one).
            if (ring._ringbuffer.empty())
                return false;
            // `chunk` is deliberately NOT decremented across producers — see the note above.
            // `scratch` is rewritten from index 0 by each producer's copy-out.
            nb_consume += ring._ringbuffer.dequeue(func, scratch, chunk);
            return false;
        });
        return nb_consume;
    }

    /**
     * @brief Get direct access to a specific producer's ring buffer
     *
     * @param index The index of the producer
     * @return Reference to the producer's ring buffer -- constructed by this call when its producer has
     *         not enqueued yet, exactly as that first enqueue would (and, on Windows, with its `std::bad_alloc`)
     */
    auto &
    ringOf(size_t const index) {
        return producer(index)._ringbuffer;
    }

    /**
     * @brief Producer rings constructed so far: one per index that has enqueued, or that `ringOf` handed out.
     * @details Any thread, no lock. A ring being constructed concurrently may or may not be counted yet: a reading of
     *          what the buffer holds, never a synchronisation point. Construction leaves it at 0.
     */
    [[nodiscard]] size_t
    rings() const noexcept {
        size_t allocated = 0;
        for (size_t i = 0; i < _nb_producer; ++i)
            if (usable(slot(i).load(std::memory_order_relaxed)))
                ++allocated;
        return allocated;
    }
};

/**
 * @brief Multi-Producer Single-Consumer ring buffer whose every producer ring is built by its constructor, in one
 * block: the engine's mailbox (Huly QB-203).
 *
 * The runtime-producer `ringbuffer<T, max_size, 0>` as the engine had it until 3.3 -- one ring per producer, the
 * consumer walking them all, a producer writing its own -- with what it held at rest cut to the rings' headers. The
 * rings live in one block of `nb_producer` rings at their natural stride, the layout the vector of rings had; each
 * is constructed by default-initialisation (placement `new Producer`, never `Producer()`), its lock and four index
 * blocks written -- a page per ring, two for the one ring in eight whose 640-byte header straddles a page -- and its
 * slot array left to its producer's writes. Every access is the vector's: a first ring (`_rings`) and an index, no
 * slot, no test. At rest an engine of N cores holds N x N ring headers, a page each and two for one in eight: 18 MiB
 * at 64 cores, 72 MiB at 128, where the value-initialised rings of 3.2 held 258 MiB and 1 GiB.
 *
 * - **POSIX.** The block is a `region_cache` region (`region.h`, Huly QB-1012), a mapping whose pages are the
 *   process's once written. On a kernel whose transparent huge pages are `always`, a header written every 66 176
 *   bytes may be backed by a 2 MiB page each time: the block, then, is resident nearly whole, as in 3.2.
 * - **Windows.** A committed mapping commits every page of a block, written or not, so the block is RESERVED
 *   (`region_acquire_reserved`): its address range is the buffer's, its pages are not. The constructor commits each
 *   ring's header pages (`region_commit`) and builds the ring CLOSED (`spsc::ringbuffer::close`): full to its
 *   producer, empty to its consumer. The producer's first write -- any producing form, `write_room`, or a write
 *   through `ringOf` -- takes the slow path every producer takes when its snapshot cannot prove room, finds the ring
 *   closed there, commits the ring's slots and opens it; nothing on a write that fits tests anything new. A refused
 *   commit terminates, as a failed allocation on the `noexcept` send path does. At rest: the header pages committed
 *   (18 MiB at 64 cores) and the block's range reserved (271 MB at 64 cores, 1.1 GB at 128).
 * - **One producer per ring.** The engine gives each core its own index into every mailbox, so a ring's first write
 *   has one writer and two threads never open one ring. The round-robin forms open a ring under its producer's lock.
 * - **Restart.** The block goes back to the region cache when the buffer is destroyed, its committed pages with it,
 *   and the next buffer of the same size takes the one released last (`SharedCoreCommunication` releases its
 *   mailboxes in the reverse of the order it built them, so each core's mailbox has its own block back).
 *
 * @tparam T The type of elements stored in the buffer
 * @tparam max_size The maximum capacity per producer buffer
 */
template <typename T, std::size_t max_size>
class block_ringbuffer : public nocopy {
    using size_t = std::size_t;

    /// A producer's ring and its lock, the line the round-robin forms take.
    struct Producer {
        constexpr static const int    padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
        SpinLock                      lock;                     ///< Lock for this producer
        char                          padding1[padding_size]{}; ///< Padding to avoid false sharing
        spsc::ringbuffer<T, max_size> _ringbuffer;              ///< The producer's dedicated ring buffer
    };
    using Ring = spsc::ringbuffer<T, max_size>;

    /// Where a producer's ring starts: past its lock line, at the ring's alignment.
    static constexpr size_t ring_offset = (QB_LOCKFREE_CACHELINE_BYTES + alignof(Ring) - 1) / alignof(Ring) * alignof(Ring);
    static_assert(sizeof(Producer) == ring_offset + sizeof(Ring), "a producer is its lock line and its ring, nothing else");
    /// What constructing a ring writes: its lock line and the ring's index blocks -- the slots follow.
    static constexpr size_t header_bytes = ring_offset + Ring::slots_offset;
    static_assert(std::is_trivially_default_constructible_v<T>,
                  "default-initialisation leaves the slots unwritten: on Windows, in pages not committed yet");

#if defined(_WIN32) || defined(_WIN64)
    static constexpr bool reserved = true; ///< The block reserved, each ring's header committed, its slots by its first write
#else
    static constexpr bool reserved = false; ///< The block a mapping: its pages are the process's once written
#endif

    Producer *const   _rings;       ///< The block: `_nb_producer` rings at their stride, the first at its start
    const std::size_t _block_bytes; ///< The block's size, as `region_acquire_reserved` was asked for it
    const bool        _closed;      ///< The rings were built closed (`spsc::ringbuffer::close`)
    const std::size_t _nb_producer; ///< Number of producers: at offset 24, where the vector of rings had it

    [[nodiscard]] static Producer *
    take_block(size_t const nb_producer) {
        assert(nb_producer > 0 && "mpsc::block_ringbuffer requires at least one producer");
        return static_cast<Producer *>(qb::allocator::detail::region_acquire_reserved(nb_producer * sizeof(Producer)));
    }

public:
    block_ringbuffer() = delete;

    /**
     * @brief Builds every producer's ring, in one block.
     * @param nb_producer The number of producers (at least one)
     * @param closed Build the rings CLOSED, their slots committed by their producers' first writes: always on Windows,
     *        where the block is only reserved; on POSIX false by default, true to run the protocol there (a test: it
     *        commits nothing, a mapping's pages being the process's once written).
     * @throws std::bad_alloc when the block cannot be reserved or a header committed.
     */
    explicit block_ringbuffer(std::size_t const nb_producer, bool const closed = reserved)
        : _rings(take_block(nb_producer))
        , _block_bytes(nb_producer * sizeof(Producer))
        , _closed(reserved || closed)
        , _nb_producer(nb_producer) {
        for (size_t i = 0; i < _nb_producer; ++i) {
            void *const at = _rings + i;
            if (reserved && !qb::allocator::detail::region_commit(at, header_bytes)) {
                for (size_t built = 0; built < i; ++built)
                    _rings[built].~Producer();
                qb::allocator::detail::region_release_reserved(_rings, _block_bytes);
                throw std::bad_alloc();
            }
            // Default-initialisation: the lock and the index blocks written, the slots left as the block has them.
            Producer *const ring = ::new (at) Producer;
            if (_closed)
                ring->_ringbuffer.close();
        }
    }

    /**
     * @brief Destroys the rings and gives the block back, committed pages and all. No producer or consumer may still be
     *        using the buffer.
     */
    ~block_ringbuffer() noexcept {
        for (size_t i = 0; i < _nb_producer; ++i)
            _rings[i].~Producer();
        qb::allocator::detail::region_release_reserved(_rings, _block_bytes);
    }

    /**
     * @brief Enqueue an item into the ring of the producer chosen at compile time (no lock: one thread per index)
     * @tparam _Index The compile-time index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    template <size_t _Index>
    bool
    enqueue_at(T const &t) {
        return _rings[_Index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items into the ring of the producer chosen at compile time (no lock: one thread per index)
     * @tparam _Index The compile-time index of the producer
     * @tparam _All A `bool`, never an integer: true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <size_t _Index, std::same_as<bool> auto _All = true>
    size_t
    enqueue_at(T const *t, size_t const size) {
        return _rings[_Index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a runtime producer index (no lock: one producer per index)
     * @param index The runtime index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    bool
    enqueue(size_t const index, T const &t) {
        return _rings[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a runtime producer index (no lock: one producer per index)
     * @tparam _All A `bool`, never an integer: true, all items or none; false, as many as fit
     * @param index The runtime index of the producer
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(size_t const index, T const *t, size_t const size) {
        return _rings[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Enqueue an item using a per-thread round-robin producer index, under that producer's lock
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    size_t
    enqueue(T const &t) {
        static thread_local size_t tl_index = 0;
        const size_t               index    = tl_index++ % _nb_producer;
        std::lock_guard<SpinLock>  lock(_rings[index].lock);
        return _rings[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a per-thread round-robin producer index, under that producer's lock
     * @tparam _All A `bool`, never an integer: true, all items or none; false, as many as fit
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        static thread_local size_t tl_index = 0;
        const size_t               index    = tl_index++ % _nb_producer;
        std::lock_guard<SpinLock>  lock(_rings[index].lock);
        return _rings[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Dequeue multiple items from all producers into one output array; see `ringbuffer<T, max_size, 0>::dequeue`
     * @param ret Output array; each producer appends after the previous one's items
     * @param size **Total** budget across all producers
     * @return The number of items dequeued, in `[0, size]`
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            const size_t n = _rings[i]._ringbuffer.dequeue(ret, size);
            ret += n;
            size -= n;
            if (!size)
                break;
        }
        return save_size - size;
    }

    /**
     * @brief Does any producer ring hold at least one unread item? Consumer thread only; see
     *        `ringbuffer<T, max_size, 0>::has_data`. A closed ring is empty.
     */
    [[nodiscard]] bool
    has_data() const noexcept { // consumer thread only: `empty()` reads the consumer's private index
        for (std::size_t i = 0; i < _nb_producer; ++i)
            if (!_rings[i]._ringbuffer.empty())
                return true;
        return false;
    }

    /**
     * @brief Process all available items from all producers, in place; see `ringbuffer<T, max_size, 0>::consume_all`
     * @warning Splits an item spanning the wrap of a ring: use the copy-out overload for multi-slot items.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func) {
        size_t nb_consume = 0;
        for (std::size_t i = 0; i < _nb_producer; ++i)
            nb_consume += _rings[i]._ringbuffer.consume_all(func);
        return nb_consume;
    }

    /**
     * @brief Drain every producer through a scratch buffer, one batch per producer -- the overload the engine drains
     *        its mailboxes with; see `ringbuffer<T, max_size, 0>::consume_all(func, scratch, chunk)`
     */
    template <typename Func>
    size_t
    consume_all(Func const &func, T *scratch, size_t const chunk) {
        size_t nb_consume = 0;
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            // The poll, inline (Huly QB-257): a ring whose published write index is where this consumer's read index
            // stands has nothing to copy out, and the copy-out below is a call per ring on MSVC -- on every pass of every
            // core, for rings that are empty on every idle pass. `empty()` reads the published line `dequeue` reads
            // first; a producer's store this relaxed load does not see yet is seen by the next pass, exactly as the
            // acquire load inside `dequeue` would have seen it then (read-read coherence).
            if (_rings[i]._ringbuffer.empty())
                continue;
            // `chunk` is deliberately NOT decremented across producers; `scratch` is rewritten by each copy-out.
            nb_consume += _rings[i]._ringbuffer.dequeue(func, scratch, chunk);
        }
        return nb_consume;
    }

    /**
     * @brief Direct access to a producer's ring, a producer-side handle: the SPSC contract applies to what you do with
     *        it.
     * @details A closed ring is handed out closed, and nothing is tested here: every member of `spsc::ringbuffer` that
     *          writes a slot -- both `enqueue` forms and `write_room` -- passes through the slow path that opens it, so
     *          the ring's first write through this handle commits its slots, as an `enqueue` on this buffer would.
     *          `SharedCoreCommunication::send_run` takes a ring this way on every run.
     * @param index The index of the producer
     */
    auto &
    ringOf(size_t const index) {
        return _rings[index]._ringbuffer;
    }

    /// The rings this buffer holds: every producer's, built by the constructor.
    [[nodiscard]] size_t
    rings() const noexcept {
        return _nb_producer;
    }

    /// Were the rings built closed, their slots committed by their producers' first writes? Always on Windows.
    [[nodiscard]] bool
    closed() const noexcept {
        return _closed;
    }

    /// The block's first byte: producer `i`'s lock line is `block_bytes() / rings()` bytes times `i` past it.
    [[nodiscard]] void const *
    block() const noexcept {
        return _rings;
    }

    /// The block's size: every ring at its stride, as `region_acquire_reserved` was asked for it.
    [[nodiscard]] size_t
    block_bytes() const noexcept {
        return _block_bytes;
    }
};

} // namespace qb::lockfree::mpsc

#endif // QB_LOCKFREE_MPSC_H
