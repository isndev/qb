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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
#include <chrono>
#include <mutex>
#include "spinlock.h"
#include "spsc.h"

namespace qb::lockfree::mpsc {

/**
 * @brief High-resolution clock type used for distribution of producer indexes
 */
using Clock = std::chrono::high_resolution_clock;

/**
 * @brief Nanosecond duration type used for time measurements
 */
using Nanoseconds = std::chrono::nanoseconds;

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
    typedef std::size_t size_t;

    /**
     * @brief Producer data structure containing a lock and dedicated ring buffer
     *
     * Each producer has its own SPSC ring buffer and a lock to protect it when needed.
     * Cache line padding is used to avoid false sharing between producers.
     */
    struct Producer {
        constexpr static const int padding_size =
            QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
        SpinLock lock;                     ///< Lock for this producer
        char     padding1[padding_size]{}; ///< Padding to avoid false sharing
        spsc::ringbuffer<T, max_size>
            _ringbuffer; ///< The producer's dedicated ring buffer
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
     * @brief Enqueue an item using a random producer index
     *
     * This method automatically selects a producer based on the current time,
     * providing load balancing across producers.
     *
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    size_t
    enqueue(T const &t) {
        const size_t index = Clock::now().time_since_epoch().count() % nb_producer;
        std::lock_guard<SpinLock> lock(_producers[index].lock);
        return _producers[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a random producer index
     *
     * This method automatically selects a producer based on the current time,
     * providing load balancing across producers.
     *
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        const size_t index = Clock::now().time_since_epoch().count() % nb_producer;
        std::lock_guard<SpinLock> lock(_producers[index].lock);
        return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Dequeue multiple items from all producers
     *
     * This method tries to dequeue items from all producers' buffers until
     * either the requested number of items is dequeued or all buffers are empty.
     *
     * @param ret Array to store the dequeued items
     * @param size Maximum number of items to dequeue
     * @return The number of items successfully dequeued
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        for (auto &producer : _producers) {
            size -= producer._ringbuffer.dequeue(ret, size);
            if (!size)
                break;
        }
        return save_size - size;
    }

    /**
     * @brief Dequeue multiple items with a function to process each item
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @param ret Array to store the dequeued items
     * @param size Maximum number of items to dequeue
     * @return The number of items successfully dequeued and processed
     */
    template <typename Func>
    size_t
    dequeue(Func const &func, T *ret, size_t const size) {
        size_t nb_consume = 0;
        for (auto &producer : _producers) {
            nb_consume += producer._ringbuffer.dequeue(func, ret, size);
        }
        return nb_consume;
    }

    /**
     * @brief Process all available items from all producers
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @return The number of items successfully processed
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
    typedef std::size_t size_t;

    /**
     * @brief Producer data structure containing a lock and dedicated ring buffer
     *
     * Each producer has its own SPSC ring buffer and a lock to protect it when needed.
     * Cache line padding is used to avoid false sharing between producers.
     */
    struct Producer {
        constexpr static const int padding_size =
            QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
        SpinLock lock;                     ///< Lock for this producer
        char     padding1[padding_size]{}; ///< Padding to avoid false sharing
        spsc::ringbuffer<T, max_size>
            _ringbuffer; ///< The producer's dedicated ring buffer
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
        , _nb_producer(nb_producer) {}

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
     * @brief Enqueue an item using a random producer index
     *
     * This method automatically selects a producer based on the current time,
     * providing load balancing across producers.
     *
     * @param t The item to enqueue
     * @return true if the item was successfully enqueued, false if the buffer was full
     */
    size_t
    enqueue(T const &t) {
        const size_t index = Clock::now().time_since_epoch().count() % _nb_producer;
        std::lock_guard<SpinLock> lock(_producers[index].lock);
        return _producers[index]._ringbuffer.enqueue(t);
    }

    /**
     * @brief Enqueue multiple items using a random producer index
     *
     * This method automatically selects a producer based on the current time,
     * providing load balancing across producers.
     *
     * @tparam _All If true, requires all items to be enqueued or none
     * @param t Array of items to enqueue
     * @param size Number of items to enqueue
     * @return The number of items successfully enqueued
     */
    template <bool _All = true>
    size_t
    enqueue(T const *t, size_t const size) {
        const size_t index = Clock::now().time_since_epoch().count() % _nb_producer;
        std::lock_guard<SpinLock> lock(_producers[index].lock);
        return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
    }

    /**
     * @brief Dequeue multiple items from all producers
     *
     * This method tries to dequeue items from all producers' buffers until
     * either the requested number of items is dequeued or all buffers are empty.
     *
     * @param ret Array to store the dequeued items
     * @param size Maximum number of items to dequeue
     * @return The number of items successfully dequeued
     */
    size_t
    dequeue(T *ret, size_t size) {
        const size_t save_size = size;
        for (size_t i = 0; i < _nb_producer; ++i) {
            size -= _producers[i]._ringbuffer.dequeue(ret, size);
            if (!size)
                break;
        }
        return save_size - size;
    }

    /**
     * @brief Dequeue multiple items with a function to process each item
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @param ret Array to store the dequeued items
     * @param size Maximum number of items to dequeue
     * @return The number of items successfully dequeued and processed
     */
    template <typename Func>
    size_t
    dequeue(Func const &func, T *ret, size_t const size) {
        size_t nb_consume = 0;
        for (size_t i = 0; i < _nb_producer; ++i) {
            nb_consume += _producers[i]._ringbuffer.dequeue(func, ret, size);
        }
        return nb_consume;
    }

    /**
     * @brief Process all available items from all producers
     *
     * @tparam Func Type of the function to process dequeued items
     * @param func Function to process each dequeued item
     * @return The number of items successfully processed
     */
    template <typename Func>
    size_t
    consume_all(Func const &func) {
        size_t nb_consume = 0;
        for (size_t i = 0; i < _nb_producer; ++i) {
            nb_consume += _producers[i]._ringbuffer.consume_all(func);
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
