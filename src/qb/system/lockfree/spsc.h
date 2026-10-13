/**
 * @file qb/system/lockfree/spsc.h
 * @brief Single-Producer Single-Consumer lockfree data structures
 *
 * This file provides lockfree data structures optimized for scenarios where exactly one thread
 * produces data and exactly one thread consumes it. The implementation focuses on performance
 * through cache-friendly design and minimal synchronization overhead.
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
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <qb/system/allocator/region.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/prefix.h>
#include <thread>
#include <type_traits>
#include <utility>

namespace qb::lockfree::spsc {
namespace internal {
/**
 * @brief Base implementation of the Single-Producer Single-Consumer ringbuffer
 *
 * This class provides the core functionality for SPSC ringbuffers, handling
 * the read and write index management and the memory operations required for
 * enqueueing and dequeueing elements.
 *
 * **Layout is the design.** Each side owns two lines — a PRIVATE one it reads and writes
 * alone, and a PUBLISHED one it only ever writes, for the other side to read — and each
 * of the four sits in its own two-line block so that no spatial-prefetch partner belongs
 * to the peer:
 *
 * | block | owner    | private line (never touched by the peer)        | published line (the peer reads it) |
 * |-------|----------|-------------------------------------------------|------------------------------------|
 * | 0 / 1 | producer | `write_index_local_`, `cached_read_index_`      | `write_index_`                     |
 * | 2 / 3 | consumer | `read_index_local_`, `cached_write_index_`      | `read_index_`                      |
 *
 * The snapshots are what make the fast path local: a producer that remembers the last
 * `read_index_` it saw can prove "not full" from its own line alone, and only re-reads
 * the consumer's published line — an `acquire` load of a cache line the consumer is
 * writing, i.e. a coherence miss whenever the consumer is active — when the snapshot
 * says full. The consumer does the same in reverse for `write_index_`. On a two-core
 * ping-pong this removed 17 % (Linux) to 28 % (Windows) of the round trip. The snapshot
 * is always a SAFE under-estimate: it can only claim less room / fewer elements than
 * really exist, and a refresh is an `acquire` load like before, so the happens-before
 * edges are the ones the plain implementation had.
 *
 * The working copies are the other half of the same rule, and they are why the published
 * index sits ALONE on its line: the side that publishes an index must never READ the line
 * it publishes on. Until 3.2 `write_index_` shared its line with the producer's snapshot,
 * so every `enqueue` began by loading both from the very line the consumer was polling —
 * and once the consumer's poll has snooped that line out of the producer's cache, that
 * load is a cross-core miss on every hop, as costly as the release fence behind the
 * publish (i9-12900K / WSL2 g++-14: the two carried equal shares of a cpu-clock profile of
 * `SharedCoreCommunication::send`; a raw two-thread ring reproduced it as +45–60 ns per
 * round trip at a random phase for ONE such load). With the working index on a private
 * line the producer touches the published line with a store and nothing else — qb's
 * two-core ping-pong went 212 → 165 ns per round trip and the 100-actor cross-core ring
 * 115 → 80 ns per hop, every same-core figure unchanged (Huly QB-184).
 *
 * The derived buffer storage begins on the block AFTER the four index blocks: the ring's
 * first slots used to share a line with `read_index_`, which the producer writes on
 * every wrap while the consumer writes its index.
 *
 * A ring can be built CLOSED (Huly QB-203) -- `closable_ringbuffer`, the ring of
 * `mpsc::block_ringbuffer` on Windows, and nothing else: full to its producer (its
 * snapshot one past its write index) and empty to its consumer, with a mark the
 * consumer's published line can never hold as an index, `~max_size`. The producer's
 * first write finds no room on its snapshot, reloads the published index as every
 * refused snapshot does, and on the mark opens the ring (`open()`): commits the slots
 * and stores the consumer's true index, 0 -- the one store a producer ever makes to the
 * consumer's line, before the consumer can have taken anything. The `closable_*` members
 * below are those writes; every other ring calls the members above them, which hold no
 * test of the mark.
 *
 * @tparam T Type of elements stored in the ringbuffer
 */
template <typename T>
class ringbuffer : public nocopy {
    static_assert(std::is_trivially_copyable_v<T>, "spsc::ringbuffer<T> requires T to be trivially copyable "
                                                   "because bulk enqueue/dequeue use std::memcpy");
    using size_t                            = std::size_t;
    constexpr static const size_t cacheline = QB_LOCKFREE_CACHELINE_BYTES;
    /// A line and its spatial-prefetch partner: what each of the four index lines owns outright.
    constexpr static const size_t block = 2 * cacheline;
    static_assert(cacheline >= 4 * sizeof(size_t), "a cache line must hold two indices with room to spare");

    /* --- producer PRIVATE block: read and written by the enqueue thread, never touched by the
     *     dequeue thread. The working write index and the snapshot of the consumer's index. --- */
    alignas(block) size_t write_index_local_;
    size_t cached_read_index_; // last read_index_ the producer saw
    char   padding0_[block - 2 * sizeof(size_t)]{};
    /* --- producer PUBLISHED block: written by the enqueue thread, polled by the dequeue thread.
     *     The producer never READS it — see the class note. --- */
    alignas(block) std::atomic<size_t> write_index_;
    char padding1_[block - sizeof(size_t)]{};
    /* --- consumer PRIVATE block: the working read index and the snapshot of the producer's. --- */
    alignas(block) size_t read_index_local_;
    size_t cached_write_index_; // last write_index_ the consumer saw
    char   padding2_[block - 2 * sizeof(size_t)]{};
    /* --- consumer PUBLISHED block: written by the dequeue thread, read by the enqueue thread
     *     when its snapshot cannot prove room. --- */
    alignas(block) std::atomic<size_t> read_index_;
    char padding3_[block - sizeof(size_t)]{};

protected:
    /**
     * @brief Default constructor initializing indices
     */
    ringbuffer()
        : write_index_local_(0)
        , cached_read_index_(0)
        , write_index_(0)
        , read_index_local_(0)
        , cached_write_index_(0)
        , read_index_(0) {}

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
     * @brief Producer side: slots writable RIGHT NOW, refreshing the read-index snapshot only
     *        when the snapshot cannot prove @p wanted
     *
     * The primitive behind a bulk producer that sizes its write to the ring: ask for the run it
     * would like to write, and receive either at least that (proved from the producer's own line)
     * or the refreshed truth. A subsequent `enqueue<true>` of at most the returned count cannot
     * fail — the consumer only ever frees slots. The snapshot policy is the same as `enqueue`'s,
     * so calling this before an `enqueue` costs no cross-line read the `enqueue` would not have
     * paid itself.
     *
     * @param wanted Number of slots the caller needs the answer to be at least
     * @param max_size Maximum size of the buffer
     * @return Number of slots available for writing (a safe under-estimate unless refreshed)
     */
    [[nodiscard]] size_t
    write_room(size_t const wanted, size_t const max_size) noexcept {
        return producer_available(write_index_local_, wanted, max_size);
    }

    // ---- a ring built closed (`closable_ringbuffer`, Huly QB-203) -------------------------------------------------

    /// Is this published read index the mark of a ring built closed? The mark is `~max_size`, above every index.
    [[nodiscard]] static constexpr bool
    closed_mark(size_t const read_index, size_t const max_size) noexcept {
        return read_index >= max_size;
    }

    /**
     * @brief Builds the ring CLOSED: full to its producer, empty to its consumer, until the producer's first write
     *        opens it. A fresh ring only (no write yet).
     * @details The producer's snapshot of the consumer's index is set where a full ring would have it, so its first
     *          write -- a `closable_enqueue` or `closable_write_room` -- takes the slow path every producer takes when
     *          its snapshot cannot prove room; there the published read index, which holds the mark, sends it to
     *          `open()`. The consumer reads the published write index, still 0: an empty ring.
     */
    void
    close(size_t const max_size) noexcept {
        cached_read_index_ = next_index(write_index_local_, max_size);
        read_index_.store(~max_size, std::memory_order_relaxed);
    }

    /**
     * @brief Opens a closed ring: commits its slots (`region_commit`: Windows commits the pages a reserved block left
     *        uncommitted, POSIX has nothing to do), then gives the producer its true snapshot. The producer's own
     *        thread, once.
     * @details The producer writes the consumer's published line here, the one time it ever does: the consumer
     *          publishes its read index only after taking an item, and there is none before this write. A refused
     *          commit cannot be reported from a send path that is `noexcept`: it terminates, as a failed allocation
     *          there does.
     * @return The room the ring now has, every slot: a closed ring was never written.
     */
    QB_NOINLINE QB_COLD size_t
    open(T *const buffer, size_t const max_size) noexcept {
        if (!qb::allocator::detail::region_commit(buffer, max_size * sizeof(T)))
            std::terminate();
        cached_read_index_ = 0;
        read_index_.store(0, std::memory_order_relaxed);
        return write_available(write_index_local_, 0, max_size);
    }

    /// `producer_available` for a ring that may be closed: the mark opens it, and the room it then has is returned,
    /// so the caller keeps nothing live across the cold call. Forced inline: MSVC gives up a function the call made
    /// bigger, and a call per write is what the closed protocol must not cost.
    QB_MSVC_FORCEINLINE size_t
    closable_producer_available(size_t const write_index, size_t const wanted, T *const buffer, size_t const max_size) {
        size_t avail = write_available(write_index, cached_read_index_, max_size);
        if (avail < wanted) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire); // the one cross-line read
            if (unlikely(closed_mark(cached_read_index_, max_size))) [[unlikely]]
                return open(buffer, max_size);
            avail = write_available(write_index, cached_read_index_, max_size);
        }
        return avail;
    }

    /// `write_room` for a ring that may be closed: its first call opens it.
    [[nodiscard]] size_t
    closable_write_room(size_t const wanted, T *const buffer, size_t const max_size) noexcept {
        return closable_producer_available(write_index_local_, wanted, buffer, max_size);
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
        const size_t write_index = write_index_local_; // the producer's own copy: never the published line
        const size_t next        = next_index(write_index, max_size);

        if (next == cached_read_index_) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (next == cached_read_index_)
                return false; /* ringbuffer is full */
        }

        new (buffer + write_index) T(t); // copy-construct

        write_index_local_ = next;
        write_index_.store(next, std::memory_order_release);

        return true;
    }

    /**
     * @brief Enqueue multiple elements into the buffer
     *
     * @tparam _All If true, either all elements are enqueued or none; a `bool`, never an integer (Huly QB-980)
     * @param input_buffer Pointer to source elements
     * @param input_count Number of elements to enqueue
     * @param internal_buffer Pointer to the internal buffer
     * @param max_size Maximum size of the buffer
     * @return Number of elements successfully enqueued
     */
    template <std::same_as<bool> auto _All>
    size_t
    enqueue(const T *input_buffer, size_t input_count, T *internal_buffer, size_t const max_size) {
        const size_t write_index = write_index_local_; // the producer's own copy: never the published line
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

        write_index_local_ = new_write_index;
        write_index_.store(new_write_index, std::memory_order_release);
        return input_count;
    }

    /// `enqueue(t, buffer, max_size)` for a ring that may be closed: the mark opens it, then the item goes in. Nothing
    /// of the item is handed to the cold call -- an item passed by reference to an out-of-line function is an item
    /// the caller must keep in memory on every write, and MSVC copies it there on the fast path too.
    bool
    closable_enqueue(T const &t, T *buffer, size_t const max_size) {
        const size_t write_index = write_index_local_; // the producer's own copy: never the published line
        const size_t next        = next_index(write_index, max_size);

        if (next == cached_read_index_) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (unlikely(closed_mark(cached_read_index_, max_size))) [[unlikely]]
                (void) open(buffer, max_size); // the snapshot is 0 now and `next` is 1: the item goes in below
            if (next == cached_read_index_)
                return false; /* full */
        }

        new (buffer + write_index) T(t); // copy-construct

        write_index_local_ = next;
        write_index_.store(next, std::memory_order_release);

        return true;
    }

    /// A closed ring's first bulk write: opens it, then copies into the fresh ring from slot 0 (no wrap), all or none
    /// per `_All`. The bulk write's fast path tail-calls this with four register arguments -- the slot count is read
    /// back from the mark -- and it calls no producing form back.
    template <std::same_as<bool> auto _All>
    QB_NOINLINE QB_COLD size_t
    open_and_enqueue(const T *input_buffer, size_t input_count, T *const internal_buffer) noexcept {
        size_t const room = open(internal_buffer, ~cached_read_index_);
        if constexpr (_All) {
            if (room < input_count)
                return 0;
        } else {
            input_count = (std::min) (input_count, room);
            if (!input_count)
                return 0;
        }
        std::memcpy(internal_buffer, input_buffer, input_count * sizeof(T));
        write_index_local_ = input_count;
        write_index_.store(input_count, std::memory_order_release);
        return input_count;
    }

    /// `enqueue<_All>(input, count, buffer, max_size)` for a ring that may be closed.
    template <std::same_as<bool> auto _All>
    size_t
    closable_enqueue(const T *input_buffer, size_t input_count, T *internal_buffer, size_t const max_size) {
        const size_t write_index = write_index_local_; // the producer's own copy: never the published line
        size_t       avail       = write_available(write_index, cached_read_index_, max_size);
        if (avail < input_count) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire); // the one cross-line read
            if (unlikely(closed_mark(cached_read_index_, max_size))) [[unlikely]]
                return open_and_enqueue<_All>(input_buffer, input_count, internal_buffer);
            avail = write_available(write_index, cached_read_index_, max_size);
        }

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

        write_index_local_ = new_write_index;
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
        const size_t read_index = read_index_local_; // the consumer's own copy: never the published line
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

        read_index_local_ = new_read_index;
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
        const size_t read_index = read_index_local_; // the consumer's own copy: never the published line
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

        read_index_local_ = new_read_index;
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
        return *(internal_buffer + read_index_local_); // consumer side: its own copy
    }

    /**
     * @brief Get a reference to the element at the read index
     *
     * @param internal_buffer Pointer to the internal buffer
     * @return Reference to the front element
     */
    T &
    front(T *internal_buffer) {
        return *(internal_buffer + read_index_local_); // consumer side: its own copy
    }

public:
    /**
     * @brief Consumer-side poll: is there nothing to dequeue right now?
     * @details The consumer's own index comes from its private line; the producer's from the
     *          published one — the only line of the peer's this poll ever reads. Consumer thread only.
     * @return true when the consumer has nothing to dequeue.
     */
    [[nodiscard]] bool
    empty() const noexcept {
        return write_index_.load(std::memory_order_relaxed) == read_index_local_;
    }

    /**
     * @brief Producer-side: the consumer's index as the producer last saw it -- its own snapshot, no
     *        cross-line read.
     * @details Refreshed by every enqueue the snapshot could not prove room for, so right after a
     *          refused enqueue it is the consumer's published index at that moment. A producer that
     *          finds the ring full twice and reads the same value here knows the consumer took
     *          nothing in between: the stall test of the core's flush (Huly QB-979). Producer thread only.
     */
    [[nodiscard]] size_t
    producer_seen_read_index() const noexcept {
        return cached_read_index_;
    }
};
static_assert(sizeof(ringbuffer<int>) == 8 * QB_LOCKFREE_CACHELINE_BYTES,
              "spsc::internal::ringbuffer is four two-line blocks: each side's private line and published line");
static_assert(alignof(ringbuffer<int>) == 2 * QB_LOCKFREE_CACHELINE_BYTES,
              "spsc::internal::ringbuffer starts on a two-line block so the derived storage does too");

template <typename T, std::size_t Slots>
class closable_ringbuffer;

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
    template <typename, std::size_t>
    friend class internal::closable_ringbuffer; // the ring of mpsc::block_ringbuffer, built closed (Huly QB-203)

public:
    /**
     * @brief Producer side: slots writable right now — see `internal::ringbuffer::write_room`
     * @param wanted Number of slots the caller needs the answer to be at least
     * @return Number of slots a following `enqueue<true>` of that many cannot fail on
     */
    [[nodiscard]] size_t
    write_room(size_t const wanted) noexcept {
        return internal::ringbuffer<T>::write_room(wanted, max_size);
    }

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
     * @tparam _All If true, either all elements are enqueued or none; a `bool`, never an integer: `enqueue<0>(t, size)` does not compile
     * @param t Pointer to source elements
     * @param size Number of elements to enqueue
     * @return Number of elements successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
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
    dequeue(Func const &func, T *ret,
            size_t size) noexcept(noexcept(std::declval<Func const &>()(std::declval<T *>(), std::declval<size_t>()))) {
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
     * @brief Producer side: slots writable right now — see `internal::ringbuffer::write_room`
     * @param wanted Number of slots the caller needs the answer to be at least
     * @return Number of slots a following `enqueue<true>` of that many cannot fail on
     */
    [[nodiscard]] size_t
    write_room(size_t const wanted) noexcept {
        return internal::ringbuffer<T>::write_room(wanted, max_size_);
    }

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
     * @tparam _All If true, either all elements are enqueued or none; a `bool`, never an integer: `enqueue<0>(t, size)` does not compile
     * @param t Pointer to source elements
     * @param size Number of elements to enqueue
     * @return Number of elements successfully enqueued
     */
    template <std::same_as<bool> auto _All = true>
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
    dequeue(Func const &func, T *ret,
            size_t size) noexcept(noexcept(std::declval<Func const &>()(std::declval<T *>(), std::declval<size_t>()))) {
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

namespace internal {

/**
 * @brief The fixed-size ring, buildable CLOSED: the ring of `mpsc::block_ringbuffer<T, N, true>` -- the engine's
 *        mailbox on Windows, where its slots are reserved pages until its producer's first write (Huly QB-203).
 * @details Every producing member opens a closed ring on its first write -- the base's class note says how -- and is
 *          otherwise the fixed ring's; the consuming members are the fixed ring's. No other type tests the closed
 *          mark: a ring that cannot be closed carries none of this.
 */
/// The fixed ring a closable one extends.
template <typename T, std::size_t Slots>
using fixed_ring = spsc::ringbuffer<T, Slots>;

template <typename T, std::size_t Slots>
class closable_ringbuffer : public fixed_ring<T, Slots> {
    using base   = fixed_ring<T, Slots>;
    using size_t = std::size_t;

public:
    /// The bytes before the slots: the four index blocks (eight lines, all a constructor writes). The slots follow.
    static constexpr size_t slots_offset = (8 * QB_LOCKFREE_CACHELINE_BYTES + alignof(std::array<T, base::max_size>) - 1)
                                           / alignof(std::array<T, base::max_size>) * alignof(std::array<T, base::max_size>);

    /// Builds this fresh ring CLOSED until its producer's first write -- see the index blocks' `close`.
    void
    close() noexcept {
        base::close(base::max_size);
    }

    /// Producer side: slots writable right now; the first call on a closed ring opens it.
    [[nodiscard]] size_t
    write_room(size_t const wanted) noexcept {
        return base::closable_write_room(wanted, this->array_.data(), base::max_size);
    }

    /// Enqueue a single element; the first write on a closed ring opens it.
    inline bool
    enqueue(T const &t) noexcept {
        return base::closable_enqueue(t, this->array_.data(), base::max_size);
    }

    /// Enqueue multiple elements, all or none per `_All`; the first write on a closed ring opens it.
    template <std::same_as<bool> auto _All = true>
    inline size_t
    enqueue(T const *t, size_t size) noexcept {
        return base::template closable_enqueue<_All>(t, size, this->array_.data(), base::max_size);
    }
};

} // namespace internal

} // namespace qb::lockfree::spsc

#endif /* QB_LOCKFREE_SPSC_H */
