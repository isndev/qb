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
#include <mutex>
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
 * **A producer's ring exists once that producer has used it** (Huly QB-203). Construction allocates one pointer per
 * producer and nothing else; the first enqueue on an index -- or `ringOf(index)` -- allocates that producer's ring and
 * publishes it, and the consumer's walks skip an index whose ring was never published. Memory is therefore proportional to
 * the producers that have produced, not to the producers that might. Until 3.3 the rings were a `std::vector<Producer>` of
 * `nb_producer` elements, value-initialised: every byte of every ring -- a `max_size + 1` slot array each -- was written
 * at construction. The engine builds one of these per core with one producer per core, so an engine of N cores held
 * N² rings before its first event: 66 176 bytes each with the engine's event buckets, 1 GiB at 128 cores, almost all
 * of it for pairs of cores that never talk.
 *
 * - **Storage.** A ring comes from `qb::allocator::region_allocator` (`region.h`, Huly QB-1012): from 64 KiB -- the
 *   engine's rings are 66 176 bytes -- a `region_cache` region, below it the global allocator. Destroyed, a region-backed
 *   ring stays mapped in that cache, so the next ring of its size -- the next engine's, in a process that restarts its
 *   engine -- is built in pages already faulted, until `region_cache::trim()` gives them back to the OS.
 * - **Placement.** A region is page-aligned, so rings placed at the start of theirs would put the same line of every ring
 *   -- the consumer's read index, the producer's write index -- at the same offset in a page: one L1 set for all of them,
 *   which a consumer walking more rings than the set has ways misses on every walk, and the same low twelve address bits,
 *   which the store buffer cannot tell apart until a store commits (4K aliasing -- what `segmented_pipe` staggers its
 *   segments against). The vector of rings this replaced spread them by its 66 176-byte stride, 640 bytes in a page. So
 *   a ring starts `((index + s) % slots) * alignof(Producer)` bytes into its region, `s` taken per buffer from a counter,
 *   in the bytes the region's page rounding leaves anyway: 28 slots for the engine's rings in their 17-page regions; the
 *   producers of one buffer, and one producer across consecutive buffers -- an engine's mailboxes, built in core order --
 *   one slot apart. Every ring of a type takes the same size of region, whatever its slot, so the cache reuses any.
 * - **Publication.** The allocation is default-initialisation (placement `new Producer`, never `Producer()`): the lock
 *   and the ring's four index blocks are constructed, the slot array is left as the allocator hands it -- the pages of
 *   a fresh region the producer has not written yet are pages the process does not hold yet. The ring is published by a
 *   compare-exchange (release) of its slot from null; every reader of a slot loads it with acquire, so the indices the
 *   constructor wrote happen-before any use of the ring on another thread. One thread per index never loses the
 *   exchange; the round-robin forms and `ringOf` may race the index's owner, and the loser gives its unpublished ring
 *   back and uses the winner's.
 * - **What each path pays.** A producer: one load of its slot and a branch it predicts after the first enqueue, where it
 *   used to compute an address in the vector; the allocation itself is out of line. The consumer: one load per producer
 *   per walk -- a cache line holds eight slots with 64-byte lines, and the slot array is whole lines so nothing else
 *   shares them -- and an index with no ring costs that load and nothing more, where it used to cost the two lines of a
 *   ring's header.
 * - **Failure.** The first enqueue on an index allocates, so it can throw `std::bad_alloc`; every later one cannot.
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

    std::unique_ptr<SlotLine[]> _slots;       ///< One slot per producer, `slots_per_line` to a line
    const std::size_t           _nb_producer; ///< Number of producers
    const std::size_t           _stagger;     ///< This buffer's first stagger slot: see `stagger_of`

    /// Consecutive buffers -- an engine's mailboxes, built in core order -- take consecutive first slots.
    [[nodiscard]] static size_t
    next_stagger() noexcept {
        static constinit std::atomic<size_t> built{0};
        return built.fetch_add(1, std::memory_order_relaxed) % stagger_slots;
    }

    /// Where producer @p index's ring starts in its storage: one slot further for each producer, and for each buffer.
    [[nodiscard]] size_t
    stagger_of(size_t const index) const noexcept {
        return (index + _stagger) % stagger_slots * alignof(Producer);
    }

    [[nodiscard]] Slot &
    slot(size_t const index) const noexcept {
        return _slots[index / slots_per_line].entry[index % slots_per_line];
    }

    /// The ring producer @p index has published, or null: what every consumer walk reads.
    [[nodiscard]] Producer *
    published(size_t const index) const noexcept {
        return slot(index).load(std::memory_order_acquire);
    }

    /// The ring of producer @p index, allocated by the first call that needs it.
    [[nodiscard]] Producer &
    producer(size_t const index) {
        if (Producer *const ring = published(index); likely(ring != nullptr)) [[likely]]
            return *ring;
        return allocate_producer(index);
    }

    // The cold half of `producer()`, out of line so that an enqueue path inlining the hot half keeps its registers.
    [[nodiscard]] QB_NOINLINE QB_COLD Producer &
    allocate_producer(size_t const index) {
        void *storage;
        if constexpr (ring_in_region)
            storage = qb::allocator::region_allocator<unsigned char>{}.allocate(ring_storage) + stagger_of(index);
        else
            storage = qb::allocator::region_allocator<Producer>{}.allocate(1);
        // Default-initialisation -- placement `new Producer`, never `Producer()` and never `make_region`, both of which
        // value-initialise: the slot array stays untouched. See the class note.
        Producer *const fresh  = ::new (storage) Producer;
        Producer       *winner = nullptr;
        if (slot(index).compare_exchange_strong(winner, fresh, std::memory_order_acq_rel, std::memory_order_acquire))
            return *fresh;
        release(index, fresh); // another thread published this index's ring first; it was never seen by anyone
        return *winner;
    }

    /// Destroys the ring `allocate_producer(index)` made and gives its storage back: a region to the region cache.
    void
    release([[maybe_unused]] size_t const index, Producer *const ring) const noexcept {
        ring->~Producer();
        if constexpr (ring_in_region)
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
     * @details Allocates one slot per producer and no ring: a producer's ring is allocated by its first enqueue.
     */
    explicit ringbuffer(std::size_t const nb_producer)
        : _slots(std::make_unique<SlotLine[]>((nb_producer + slots_per_line - 1) / slots_per_line))
        , _nb_producer(nb_producer)
        , _stagger(next_stagger()) {
        // At least one producer is required: the round-robin enqueue paths
        // compute `tl_index % _nb_producer`, which is division-by-zero (UB)
        // when constructed with zero producers.
        assert(nb_producer > 0 && "mpsc::ringbuffer requires at least one producer");
    }

    /**
     * @brief Frees every ring a producer allocated. No producer or consumer may still be using the buffer.
     */
    ~ringbuffer() noexcept {
        for (size_t i = 0; i < _nb_producer; ++i)
            if (Producer *const ring = slot(i).load(std::memory_order_relaxed))
                release(i, ring);
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
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            Producer *const ring = published(i);
            if (!ring)
                continue; // never enqueued into: nothing to take
            // Advance ret by the amount taken so each producer appends after
            // the previous one's items instead of overwriting them (silent
            // data loss: items consumed from the ring but lost in the output).
            const size_t n = ring->_ringbuffer.dequeue(ret, size);
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
        for (std::size_t i = 0; i < _nb_producer; ++i)
            if (Producer const *const ring = published(i); ring && !ring->_ringbuffer.empty())
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
     *          logical item spanning several slots (a multi-bucket `qb::Event` is exactly
     *          that). For those, use the copy-out overload below.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func) {
        size_t nb_consume = 0;
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            if (Producer *const ring = published(i))
                nb_consume += ring->_ringbuffer.consume_all(func);
        }
        return nb_consume;
    }

    /**
     * @brief Drain every producer through a scratch buffer, one batch per producer
     *
     * Calls `func(scratch, n)` once per non-empty producer ring, where `n` is the number of
     * items copied out of *that* ring. Unlike `consume_all(func)` above, each batch is
     * copied out contiguously first, so an item spanning several slots survives a wrap —
     * which is why this is the overload the engine drains mailboxes with.
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
     *       `SharedCoreCommunication`'s teardown sweep both depend on the per-producer form:
     *       a budget shared across the loop would let one saturated producer consume it and
     *       starve every other core's ring. For a bounded total, call `dequeue(T*, size)`.
     */
    template <typename Func>
    size_t
    consume_all(Func const &func, T *scratch, size_t const chunk) {
        size_t nb_consume = 0;
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            // The poll, inline (Huly QB-257): a ring whose published write index is where this
            // consumer's read index stands has nothing to copy out, and the copy-out below is a call
            // per ring on MSVC -- on every pass of every core, for rings that are empty on every
            // idle pass. `empty()` reads the published line `dequeue` reads first; a producer's
            // store this relaxed load does not see yet is seen by the next pass, exactly as the
            // acquire load inside `dequeue` would have seen it then (read-read coherence: the
            // acquire load never reads an older value than this one).
            if (_producers[i]._ringbuffer.empty())
                continue;
            // `chunk` is deliberately NOT decremented across producers — see the note above.
            // `scratch` is rewritten from index 0 by each producer's copy-out. A producer that has
            // never enqueued has no ring to drain.
            if (Producer *const ring = published(i))
                nb_consume += ring->_ringbuffer.dequeue(func, scratch, chunk);
        }
        return nb_consume;
    }

    /**
     * @brief Get direct access to a specific producer's ring buffer
     *
     * @param index The index of the producer
     * @return Reference to the producer's ring buffer -- allocated by this call when its producer has
     *         not enqueued yet, exactly as that first enqueue would (and with the same `std::bad_alloc`)
     */
    auto &
    ringOf(size_t const index) {
        return producer(index)._ringbuffer;
    }

    /**
     * @brief Producer rings allocated so far: one per index that has enqueued, or that `ringOf` handed out.
     * @details Any thread, no lock. A ring being allocated concurrently may or may not be counted yet: a reading of
     *          what the buffer holds, never a synchronisation point. Construction leaves it at 0.
     */
    [[nodiscard]] size_t
    rings() const noexcept {
        size_t allocated = 0;
        for (size_t i = 0; i < _nb_producer; ++i)
            if (slot(i).load(std::memory_order_relaxed) != nullptr)
                ++allocated;
        return allocated;
    }
};

} // namespace qb::lockfree::mpsc

#endif // QB_LOCKFREE_MPSC_H
