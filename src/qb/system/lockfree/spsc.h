/**
 * @file qb/system/lockfree/spsc.h
 * @brief Single-Producer Single-Consumer lockfree data structures
 *
 * This file provides lockfree data structures optimized for scenarios where
 * exactly one thread produces data and exactly one thread consumes it.
 * The implementation focuses on performance through cache-friendly design
 * and minimal synchronization overhead.
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

#ifndef QB_LOCKFREE_SPSC_H
#define QB_LOCKFREE_SPSC_H
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <qb/utility/branch_hints.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/prefix.h>
#include <thread>
#include <type_traits>

namespace qb::lockfree::spsc {
namespace internal {
/**
 * @brief Base implementation of the Single-Producer Single-Consumer ringbuffer
 *
 * This class provides the core functionality for SPSC ringbuffers, handling
 * the read and write index management and the memory operations required for
 * enqueueing and dequeueing elements.
 *
 * **Layout is the design.** Each side owns exactly one cache line and never writes
 * the other's:
 *
 * | line | owner    | published index (the other side reads it) | private snapshot of the peer's index |
 * |------|----------|--------------------------------------------|--------------------------------------|
 * | 0    | producer | `write_index_`                             | `cached_read_index_`                 |
 * | 1    | consumer | `read_index_`                              | `cached_write_index_`                |
 *
 * The snapshots are what make the fast path local: a producer that remembers the last
 * `read_index_` it saw can prove "not full" from its own line alone, and only re-reads
 * the consumer's line — an `acquire` load of a cache line the consumer is writing, i.e.
 * a coherence miss whenever the consumer is active — when the snapshot says full. The
 * consumer does the same in reverse for `write_index_`. On a two-core ping-pong this
 * removed 17 % (Linux) to 28 % (Windows) of the round trip. The snapshot is always a
 * SAFE under-estimate: it can only claim less room / fewer elements than really exist,
 * and a refresh is an `acquire` load like before, so the happens-before edges are the
 * ones the plain implementation had.
 *
 * The derived buffer storage begins on the line AFTER the two index lines: the ring's
 * first slots used to share a line with `read_index_`, which the producer writes on
 * every wrap while the consumer writes its index.
 *
 * @tparam T Type of elements stored in the ringbuffer
 */
template <typename T>
class ringbuffer : public nocopy {
    static_assert(std::is_trivially_copyable_v<T>, "spsc::ringbuffer<T> requires T to be trivially copyable "
                                                   "because bulk enqueue/dequeue use std::memcpy");
    using size_t                            = std::size_t;
    constexpr static const size_t cacheline = QB_LOCKFREE_CACHELINE_BYTES;
    constexpr static const size_t line_pad  = cacheline - 2 * sizeof(size_t);
    static_assert(cacheline >= 4 * sizeof(size_t), "a cache line must hold two indices with room to spare");

    /* --- producer line: written ONLY by the enqueue thread --- */
    QB_LOCKFREE_CACHELINE_ALIGNMENT std::atomic<size_t> write_index_;
    size_t                                              cached_read_index_; // last read_index_ the producer saw
    char                                                padding0_[line_pad]{};
    /* --- consumer line: written ONLY by the dequeue thread --- */
    QB_LOCKFREE_CACHELINE_ALIGNMENT std::atomic<size_t> read_index_;
    size_t                                              cached_write_index_; // last write_index_ the consumer saw
    char                                                padding1_[line_pad]{};

protected:
    /**
     * @brief Default constructor initializing indices
     */
    ringbuffer()
        : write_index_(0)
        , cached_read_index_(0)
        , read_index_(0)
        , cached_write_index_(0) {}

    /**
     * @brief Calculate the next index in the buffer with wrap-around handling
     *
     * @param arg Current index
     * @param max_size Maximum size of the buffer
     * @return Next index with wrap-around if needed
     */
    static size_t
    next_index(size_t arg, size_t const max_size) {
        const size_t next = arg + 1;
        return next < max_size ? next : 0;
    }

    /**
     * @brief Calculate how many elements are available for reading
     *
     * @param write_index Current write index
     * @param read_index Current read index
     * @param max_size Maximum size of the buffer
     * @return Number of elements available for reading
     */
    static size_t
    read_available(size_t write_index, size_t read_index, size_t const max_size) {
        if (write_index >= read_index)
            return write_index - read_index;

        const size_t ret = write_index + max_size - read_index;
        return ret;
    }

    /**
     * @brief Calculate how many elements can be written
     *
     * @param write_index Current write index
     * @param read_index Current read index
     * @param max_size Maximum size of the buffer
     * @return Number of slots available for writing
     */
    static size_t
    write_available(size_t write_index, size_t read_index, size_t const max_size) {
        size_t ret = read_index - write_index - 1;
        if (write_index >= read_index)
            ret += max_size;
        return ret;
    }

    /**
     * @brief Get the number of elements available for reading
     *
     * Always reads the published indices (never a snapshot): this is a query, not a fast path.
     *
     * @param max_size Maximum size of the buffer
     * @return Number of elements available for reading
     */
    [[nodiscard]] size_t
    read_available(size_t const max_size) const {
        size_t       write_index = write_index_.load(std::memory_order_acquire);
        const size_t read_index  = read_index_.load(std::memory_order_relaxed);
        return read_available(write_index, read_index, max_size);
    }

    /**
     * @brief Get the number of slots available for writing
     *
     * Always reads the published indices (never a snapshot): this is a query, not a fast path.
     *
     * @param max_size Maximum size of the buffer
     * @return Number of slots available for writing
     */
    [[nodiscard]] size_t
    write_available(size_t const max_size) const {
        size_t       write_index = write_index_.load(std::memory_order_relaxed);
        const size_t read_index  = read_index_.load(std::memory_order_acquire);
        return write_available(write_index, read_index, max_size);
    }

    /**
     * @brief Producer side: slots writable at @p write_index, refreshing the read-index snapshot
     *        only when the snapshot cannot prove @p wanted slots
     *
     * @param write_index The producer's current write index
     * @param wanted Number of slots the caller needs the answer to be at least
     * @param max_size Maximum size of the buffer
     * @return Number of slots available for writing (a safe under-estimate unless refreshed)
     */
    size_t
    producer_available(size_t const write_index, size_t const wanted, size_t const max_size) {
        size_t avail = write_available(write_index, cached_read_index_, max_size);
        if (avail < wanted) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire); // the one cross-line read
            avail              = write_available(write_index, cached_read_index_, max_size);
        }
        return avail;
    }

    /**
     * @brief Consumer side: elements readable at @p read_index, refreshing the write-index
     *        snapshot only when the snapshot cannot prove @p wanted elements
     *
     * A caller that wants "everything" (`consume_all`) passes `SIZE_MAX` and therefore always
     * refreshes: its contract is every published element, not every element last seen.
     *
     * @param read_index The consumer's current read index
     * @param wanted Number of elements the caller needs the answer to be at least
     * @param max_size Maximum size of the buffer
     * @return Number of elements available for reading (a safe under-estimate unless refreshed)
     */
    size_t
    consumer_available(size_t const read_index, size_t const wanted, size_t const max_size) {
        size_t avail = read_available(cached_write_index_, read_index, max_size);
        if (avail < wanted) {
            cached_write_index_ = write_index_.load(std::memory_order_acquire); // the one cross-line read
            avail               = read_available(cached_write_index_, read_index, max_size);
        }
        return avail;
    }

    /**
     * @brief Enqueue a single element into the buffer
     *
     * @param t Element to enqueue
     * @param buffer Pointer to the internal buffer
     * @param max_size Maximum size of the buffer
     * @return true if the element was successfully enqueued, false if the buffer is full
     */
    bool
    enqueue(T const &t, T *buffer, size_t const max_size) {
        const size_t write_index = write_index_.load(std::memory_order_relaxed); // only written from enqueue thread
        const size_t next        = next_index(write_index, max_size);

        if (next == cached_read_index_) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (next == cached_read_index_)
                return false; /* ringbuffer is full */
        }

        new (buffer + write_index) T(t); // copy-construct

        write_index_.store(next, std::memory_order_release);

        return true;
    }

    /**
     * @brief Enqueue multiple elements into the buffer
     *
     * @tparam _All If true, either all elements are enqueued or none
     * @param input_buffer Pointer to source elements
     * @param input_count Number of elements to enqueue
     * @param internal_buffer Pointer to the internal buffer
     * @param max_size Maximum size of the buffer
     * @return Number of elements successfully enqueued
     */
    template <bool _All>
    size_t
    enqueue(const T *input_buffer, size_t input_count, T *internal_buffer, size_t const max_size) {
        const size_t write_index = write_index_.load(std::memory_order_relaxed); // only written from push thread
        const size_t avail       = producer_available(write_index, input_count, max_size);

        if constexpr (_All) {
            if (avail < input_count)
                return 0;
        } else {
            if (!avail)
                return 0;
            input_count = (std::min) (input_count, avail);
        }

        size_t new_write_index = write_index + input_count;

        if (write_index + input_count > max_size) {
            /* copy data in two sections */
            const size_t count0 = max_size - write_index;
            const size_t count1 = input_count - count0;

            std::memcpy(internal_buffer + write_index, input_buffer, count0 * sizeof(T));
            std::memcpy(internal_buffer, input_buffer + count0, count1 * sizeof(T));
            new_write_index -= max_size;
        } else {
            std::memcpy(internal_buffer + write_index, input_buffer, input_count * sizeof(T));

            if (new_write_index == max_size)
                new_write_index = 0;
        }

        write_index_.store(new_write_index, std::memory_order_release);
        return input_count;
    }

    /**
     * @brief Dequeue multiple elements from the buffer
     *
     * @param output_buffer Destination buffer for dequeued elements
     * @param output_count Maximum number of elements to dequeue
     * @param internal_buffer Pointer to the internal buffer
     * @param max_size Maximum size of the buffer
     * @return Number of elements successfully dequeued
     */
    size_t
    dequeue(T *output_buffer, size_t output_count, T *internal_buffer, size_t const max_size) {
        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
        const size_t avail      = consumer_available(read_index, output_count, max_size);

        if (avail == 0)
            return 0;

        output_count = (std::min) (output_count, avail);

        size_t new_read_index = read_index + output_count;

        if (read_index + output_count > max_size) {
            /* copy data in two sections */
            const size_t count0 = max_size - read_index;
            const size_t count1 = output_count - count0;

            std::memcpy(output_buffer, internal_buffer + read_index, count0 * sizeof(T));
            std::memcpy(output_buffer + count0, internal_buffer, count1 * sizeof(T));

            new_read_index -= max_size;
        } else {
            std::memcpy(output_buffer, internal_buffer + read_index, output_count * sizeof(T));

            if (new_read_index == max_size)
                new_read_index = 0;
        }

        read_index_.store(new_read_index, std::memory_order_release);
        return output_count;
    }

    /**
     * @brief Process all available elements in the buffer using a functor
     *
     * @tparam _Func Functor type
     * @param functor Function to call for each batch of elements
     * @param internal_buffer Pointer to the internal buffer
     * @param max_size Maximum size of the buffer
     * @return Number of elements processed
     *
     * @warning **Only safe when each ring slot is an independent item.** This walks the ring
     *          storage IN PLACE, so when the readable range wraps the end of the buffer it
     *          invokes @p functor TWICE, on two disjoint segments — splitting any logical item
     *          that spans several slots (e.g. a multi-bucket `qb::Event`, which the producer's
     *          `enqueue` deliberately writes with a two-section memcpy across the wrap). A
     *          consumer that parses variable-length items by an embedded size field will then
     *          read a header whose item runs past the segment. If your items can span more than
     *          one slot, use the copy-out `dequeue(func, scratch, n)` overload instead: it
     *          reassembles the batch contiguously before calling the functor.
     */
    template <typename _Func>
    size_t
    consume_all(_Func const &functor, T *internal_buffer, size_t max_size) {
        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
        const size_t avail      = consumer_available(read_index, SIZE_MAX, max_size);

        if (avail == 0)
            return 0;

        const size_t output_count = avail;

        size_t new_read_index = read_index + output_count;

        if (read_index + output_count > max_size) {
            /* copy data in two sections */
            const size_t count0 = max_size - read_index;
            const size_t count1 = output_count - count0;

            functor(internal_buffer + read_index, count0);
            functor(internal_buffer, count1);

            new_read_index -= max_size;
        } else {
            functor(internal_buffer + read_index, output_count);

            if (new_read_index == max_size)
                new_read_index = 0;
        }

        read_index_.store(new_read_index, std::memory_order_release);
        return output_count;
    }

    /**
     * @brief Get a reference to the element at the read index (const version)
     *
     * @param internal_buffer Pointer to the internal buffer
     * @return Const reference to the front element
     */
    const T &
    front(const T *internal_buffer) const {
        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
        return *(internal_buffer + read_index);
    }

    /**
     * @brief Get a reference to the element at the read index
     *
     * @param internal_buffer Pointer to the internal buffer
     * @return Reference to the front element
     */
    T &
    front(T *internal_buffer) {
        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
        return *(internal_buffer + read_index);
    }

public:
    /**
     * @brief Check if the buffer is empty
     *
     * Reads the published indices, never a snapshot, so either thread may ask.
     *
     * @return true if the buffer is empty, false otherwise
     */
    [[nodiscard]] bool
    empty() const noexcept {
        return write_index_.load(std::memory_order_relaxed) == read_index_.load(std::memory_order_relaxed);
    }
};
static_assert(sizeof(ringbuffer<int>) == 2 * QB_LOCKFREE_CACHELINE_BYTES,
              "spsc::internal::ringbuffer must be exactly one producer line + one consumer line");
static_assert(alignof(ringbuffer<int>) == QB_LOCKFREE_CACHELINE_BYTES,
              "spsc::internal::ringbuffer must start on a cache line so the derived storage does too");

} // namespace internal

/**
 * @brief Fixed-size implementation of the SPSC ringbuffer
 *
 * This class provides a fixed-size compile-time SPSC ringbuffer implementation
 * with a buffer size specified as a template parameter.
 *
 * @tparam T Type of elements stored in the ringbuffer
 * @tparam _MaxSize Maximum number of elements that can be stored
 */
template <typename T, size_t _MaxSize>
class ringbuffer : public internal::ringbuffer<T> {
    using size_t                     = std::size_t;
    constexpr static size_t max_size = _MaxSize + 1;
    std::array<T, max_size> array_;

public:
    /**
     * @brief Enqueue a single element into the buffer
     *
     * @param t Element to enqueue
     * @return true if the element was successfully enqueued, false if the buffer is full
     */
    inline bool
    enqueue(T const &t) noexcept {
        return internal::ringbuffer<T>::enqueue(t, array_.data(), max_size);
    }

    /**
     * @brief Dequeue a single element from the buffer
     *
     * @param ret Pointer to store the dequeued element
     * @return true if an element was successfully dequeued, false if the buffer is empty
     */
    inline bool
    dequeue(T *ret) noexcept {
        return internal::ringbuffer<T>::dequeue(ret, 1, array_.data(), max_size);
    }

    /**
     * @brief Enqueue multiple elements into the buffer
     *
     * @tparam _All If true, either all elements are enqueued or none
     * @param t Pointer to source elements
     * @param size Number of elements to enqueue
     * @return Number of elements successfully enqueued
     */
    template <bool _All = true>
    inline size_t
    enqueue(T const *t, size_t size) noexcept {
        return internal::ringbuffer<T>::template enqueue<_All>(t, size, array_.data(), max_size);
    }

    /**
     * @brief Dequeue multiple elements from the buffer
     *
     * @param ret Destination buffer for dequeued elements
     * @param size Maximum number of elements to dequeue
     * @return Number of elements successfully dequeued
     */
    inline size_t
    dequeue(T *ret, size_t size) noexcept {
        return internal::ringbuffer<T>::dequeue(ret, size, array_.data(), max_size);
    }

    /**
     * @brief Dequeue multiple elements and process them with a functor
     *
     * @tparam Func Functor type
     * @param func Function to call after dequeuing elements
     * @param ret Destination buffer for dequeued elements
     * @param size Maximum number of elements to dequeue
     * @return Number of elements successfully dequeued and processed
     */
    template <typename Func>
    inline size_t
    dequeue(Func const &func, T *ret, size_t size) noexcept {
        const size_t nb_consume = internal::ringbuffer<T>::dequeue(ret, size, array_.data(), max_size);
        if (nb_consume)
            func(ret, nb_consume);
        return nb_consume;
    }

    /**
     * @brief Process all available elements in the buffer using a functor
     *
     * @tparam Func Functor type
     * @param func Function to call for each batch of elements
     * @return Number of elements processed
     */
    template <typename Func>
    inline size_t
    consume_all(Func const &func) noexcept {
        return internal::ringbuffer<T>::consume_all(func, array_.data(), max_size);
    }
};

/**
 * @brief Dynamic-size implementation of the SPSC ringbuffer
 *
 * This class provides a dynamic-size SPSC ringbuffer implementation
 * with a buffer size specified at runtime.
 *
 * @tparam T Type of elements stored in the ringbuffer
 */
template <typename T>
class ringbuffer<T, 0> : public internal::ringbuffer<T> {
    using size_t = std::size_t;
    const size_t max_size_;
    // unique_ptr<T[]> (array form): the buffer is allocated with new T[...],
    // so it must be released with delete[]. A scalar unique_ptr<T> would call
    // scalar delete on an array allocation (alloc/dealloc-size mismatch, UB).
    std::unique_ptr<T[]> array_;

public:
    /**
     * @brief Constructs a ringbuffer with the specified maximum size
     *
     * @param max_size Maximum number of elements that can be stored
     */
    explicit ringbuffer(size_t const max_size)
        : max_size_(max_size + 1)
        , array_(new T[max_size + 1]) {}

    /**
     * @brief Enqueue a single element into the buffer
     *
     * @param t Element to enqueue
     * @return true if the element was successfully enqueued, false if the buffer is full
     */
    inline bool
    enqueue(T const &t) noexcept {
        return internal::ringbuffer<T>::enqueue(t, array_.get(), max_size_);
    }

    /**
     * @brief Dequeue a single element from the buffer
     *
     * @param ret Pointer to store the dequeued element
     * @return true if an element was successfully dequeued, false if the buffer is empty
     */
    inline bool
    dequeue(T *ret) noexcept {
        return internal::ringbuffer<T>::dequeue(ret, 1, array_.get(), max_size_);
    }

    /**
     * @brief Enqueue multiple elements into the buffer
     *
     * @tparam _All If true, either all elements are enqueued or none
     * @param t Pointer to source elements
     * @param size Number of elements to enqueue
     * @return Number of elements successfully enqueued
     */
    template <bool _All = true>
    inline size_t
    enqueue(T const *t, size_t size) noexcept {
        return internal::ringbuffer<T>::template enqueue<_All>(t, size, array_.get(), max_size_);
    }

    /**
     * @brief Dequeue multiple elements from the buffer
     *
     * @param ret Destination buffer for dequeued elements
     * @param size Maximum number of elements to dequeue
     * @return Number of elements successfully dequeued
     */
    inline size_t
    dequeue(T *ret, size_t size) noexcept {
        return internal::ringbuffer<T>::dequeue(ret, size, array_.get(), max_size_);
    }

    /**
     * @brief Dequeue multiple elements and process them with a functor
     *
     * @tparam Func Functor type
     * @param func Function to call after dequeuing elements
     * @param ret Destination buffer for dequeued elements
     * @param size Maximum number of elements to dequeue
     * @return Number of elements successfully dequeued and processed
     */
    template <typename Func>
    inline size_t
    dequeue(Func const &func, T *ret, size_t size) noexcept {
        const size_t nb_consume = internal::ringbuffer<T>::dequeue(ret, size, array_.get(), max_size_);
        if (nb_consume)
            func(ret, nb_consume);
        return nb_consume;
    }

    /**
     * @brief Process all available elements in the buffer using a functor
     *
     * @tparam Func Functor type
     * @param func Function to call for each batch of elements
     * @return Number of elements processed
     */
    template <typename Func>
    inline size_t
    consume_all(Func const &func) noexcept {
        return internal::ringbuffer<T>::consume_all(func, array_.get(), max_size_);
    }
};

} // namespace qb::lockfree::spsc

#endif /* QB_LOCKFREE_SPSC_H */
