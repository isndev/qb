/**
 * @file qb/system/lockfree/mpsc.h
 * @brief Multiple-Producer Single-Consumer lockfree queue
 *
 * This file provides lockfree queue implementations that allow multiple producer
 * threads to safely enqueue items while a single consumer thread dequeues them,
 * all without using locks. These data structures are optimized for high-throughput
 * concurrent systems where multiple threads need to communicate with a single
 * processing thread.
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
#include <mutex>
#include "spinlock.h"
#include "spsc.h"

namespace qb::lockfree::mpsc {

/**
 * @brief Multi-Producer Single-Consumer ring buffer with fixed number of producers
 *
 * This implementation provides a lock-free MPSC ring buffer with a compile-time
 * fixed number of producers. Each producer has its own dedicated SPSC ring buffer,
 * eliminating contention between producers.
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
     * @brief Enqueue an item using a compile-time producer index
     *
     * @tparam _Index The compile-time index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    template <size_t _Index>
    bool
    enqueue(T const &t) {
        return _producers[_Index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a compile-time producer index
     *
     * @tparam _Index The compile-time index of the producer
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <size_t _Index, bool _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        return _producers[_Index]._ringbuffer.enqueue(t, size);
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
     * @tparam _All If true, requires all items to be enqueued or none
     * @param index The runtime index of the producer
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
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
     * This method automatically selects a producer using a per-thread
     * round-robin counter, providing even load balancing across producers.
     *
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
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
     * @details Consumer-side query for a park/unpark decision: reads each ring's published
     *          indices (`spsc::ringbuffer::empty()`) and nothing else — no snapshot, no lock.
     *          Meant to be evaluated AFTER a `std::atomic_thread_fence(seq_cst)` by the consumer
     *          that has just announced it is about to block (the Dekker half of a race-free
     *          wait), so that a producer's enqueue is either seen here or sees the announcement.
     * @return true if at least one ring is non-empty.
     */
    [[nodiscard]] bool
    has_data() const noexcept {
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
 * This specialization provides a MPSC ring buffer with a runtime-determined
 * number of producers specified during construction.
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

    std::vector<Producer> _producers;   ///< Vector of producer structures
    const std::size_t     _nb_producer; ///< Number of producers

public:
    /**
     * @brief Default constructor is deleted - must specify number of producers
     */
    ringbuffer() = delete;

    /**
     * @brief Constructor with runtime number of producers
     *
     * @param nb_producer The number of producers to create
     */
    explicit ringbuffer(std::size_t const nb_producer)
        : _producers(nb_producer)
        , _nb_producer(nb_producer) {
        // At least one producer is required: the round-robin enqueue paths
        // compute `tl_index % _nb_producer`, which is division-by-zero (UB)
        // when constructed with zero producers.
        assert(nb_producer > 0 && "mpsc::ringbuffer requires at least one producer");
    }

    /**
     * @brief Enqueue an item using a compile-time producer index
     *
     * @tparam _Index The compile-time index of the producer
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    template <size_t _Index>
    bool
    enqueue(T const &t) {
        return _producers[_Index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a compile-time producer index
     *
     * @tparam _Index The compile-time index of the producer
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <size_t _Index, bool _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
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
     * @tparam _All If true, requires all items to be enqueued or none
     * @param index The runtime index of the producer
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
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
        const size_t               index    = tl_index++ % _nb_producer;
        std::lock_guard<SpinLock>  lock(_producers[index].lock);
        return _producers[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a round-robin producer index
     *
     * This method automatically selects a producer using a per-thread
     * round-robin counter, providing even load balancing across producers.
     *
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        static thread_local size_t tl_index = 0;
        const size_t               index    = tl_index++ % _nb_producer;
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
     *          overload below — see that overload's warning. Here @p ret is the *output* and
     *          the budget is global; there @p ret is *scratch* and `size` is a per-producer
     *          chunk limit.
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        for (std::size_t i = 0; i < _nb_producer; ++i) {
            // Advance ret by the amount taken so each producer appends after
            // the previous one's items instead of overwriting them (silent
            // data loss: items consumed from the ring but lost in the output).
            const size_t n = _producers[i]._ringbuffer.dequeue(ret, size);
            ret += n;
            size -= n;
            if (!size)
                break;
        }
        return save_size - size;
    }

    /**
     * @brief Does any producer ring hold at least one unread item?
     * @details Consumer-side query for a park/unpark decision: reads each ring's published
     *          indices (`spsc::ringbuffer::empty()`) and nothing else — no snapshot, no lock.
     *          Meant to be evaluated AFTER a `std::atomic_thread_fence(seq_cst)` by the consumer
     *          that has just announced it is about to block (the Dekker half of a race-free
     *          wait), so that a producer's enqueue is either seen here or sees the announcement.
     * @return true if at least one ring is non-empty.
     */
    [[nodiscard]] bool
    has_data() const noexcept {
        for (std::size_t i = 0; i < _nb_producer; ++i)
            if (!_producers[i]._ringbuffer.empty())
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
            nb_consume += _producers[i]._ringbuffer.consume_all(func);
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
            // `chunk` is deliberately NOT decremented across producers — see the note above.
            // `scratch` is rewritten from index 0 by each producer's copy-out.
            nb_consume += _producers[i]._ringbuffer.dequeue(func, scratch, chunk);
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

} // namespace qb::lockfree::mpsc

#endif // QB_LOCKFREE_MPSC_H
