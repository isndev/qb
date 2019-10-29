/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_LOCKFREE_MPSC_H
#define QB_LOCKFREE_MPSC_H
# include <mutex>
# include <chrono>
# include "spsc.h"
# include "spinlock.h"

namespace qb {
    namespace lockfree {
        namespace mpsc {

            using Clock = std::chrono::high_resolution_clock;
            using Nanoseconds = std::chrono::nanoseconds;

            template<typename T, std::size_t max_size, size_t nb_producer = 0>
            class ringbuffer
                    : public nocopy {
                typedef std::size_t size_t;
                struct Producer
                {
                    constexpr static const int padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
                    SpinLock lock;
                    char padding1[padding_size];
                    spsc::ringbuffer<T, max_size> _ringbuffer;
                };

                std::array<Producer, nb_producer> _producers;

            public:
                template <size_t _Index>
                bool enqueue(T const &t) {
                    return _producers[_Index]._ringbuffer.enqueue(t);
                }

                template <size_t _Index, bool _All = true>
                size_t enqueue(T const *t, size_t const size) {
                    return _producers[_Index]._ringbuffer.enqueue(t, size);
                }

                bool enqueue(size_t const index, T const &t) {
                    return _producers[index]._ringbuffer.enqueue(t);
                }

                template <bool _All = true>
                size_t enqueue(size_t const index, T const *t, size_t const size) {
                    return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
                }

                size_t enqueue(T const &t) {
                    const size_t index = Clock::now().time_since_epoch().count() % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t);
                }

                template <bool _All = true>
                size_t enqueue(T const *t, size_t const size) {
                    const size_t index = Clock::now().time_since_epoch().count() % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.template enqueue<_All>(t, size);
                }

                size_t dequeue(T *ret, size_t size) {
                    const size_t save_size = size;
                    for (auto &producer : _producers) {
                        size -= producer._ringbuffer.dequeue(ret, size);
                        if (!size)
                            break;
                    }
                    return save_size - size;
                }

                template <typename Func>
                size_t dequeue(Func const &func, T *ret, size_t const size) {
                    size_t nb_consume = 0;
                    for (auto &producer : _producers) {
                        nb_consume += producer._ringbuffer.dequeue(func, ret, size);
                    }
                    return nb_consume;
                }
            };

            template<typename T, std::size_t max_size>
            class ringbuffer<T, max_size, 0>
                    : public nocopy {
                typedef std::size_t size_t;
                struct Producer
                {
                    constexpr static const int padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
                    SpinLock lock;
                    char padding1[padding_size];
                    spsc::ringbuffer<T, max_size> _ringbuffer;
                };

                std::unique_ptr<Producer> _producers;
                const std::size_t _nb_producer;
            public:
                ringbuffer() = delete;
                ringbuffer(std::size_t const nb_producer)
                        : _producers(new Producer[nb_producer])
                        , _nb_producer(nb_producer)
                {}

                template <size_t _Index>
                bool enqueue(T const &t) {
                    return _producers.get()[_Index]._ringbuffer.enqueue(t);
                }

                template <size_t _Index, bool _All = true>
                size_t enqueue(T const *t, size_t const size) {
                    return _producers.get()[_Index]._ringbuffer.enqueue<_All>(t, size);
                }

                bool enqueue(size_t const index, T const &t) {
                    return _producers.get()[index].enqueue(t);
                }

                template <bool _All = true>
                size_t enqueue(size_t const index, T const *t, size_t const size) {
                    return _producers.get()[index]._ringbuffer. template enqueue<_All>(t, size);
                }

                size_t enqueue(T const &t) {
                    const size_t index = Clock::now().time_since_epoch().count() % _nb_producer;
                    std::lock_guard<SpinLock> lock(_producers.get()[index].lock);
                    return _producers.get()[index]._ringbuffer.enqueue(t);
                }

                template <bool _All = true>
                size_t enqueue(T const *t, size_t const size) {
                    const size_t index = Clock::now().time_since_epoch().count() % _nb_producer;
                    std::lock_guard<SpinLock> lock(_producers.get()[index].lock);
                    return _producers.get()[index]._ringbuffer.template enqueue<_All>(t, size);
                }

                size_t dequeue(T *ret, size_t size) {
                    const size_t save_size = size;
                    for (size_t i = 0; i < _nb_producer; ++i) {
                        size -= _producers.get()[i]._ringbuffer.dequeue(ret, size);
                        if (!size)
                            break;
                    }
                    return save_size - size;
                }

                template <typename Func>
                size_t dequeue(Func const &func, T *ret, size_t const size) {
                    size_t nb_consume = 0;
                    for (size_t i = 0; i < _nb_producer; ++i) {
                        nb_consume += _producers.get()[i]._ringbuffer.dequeue(func, ret, size);
                    }
                    return nb_consume;
                }
            };

        } /* namespace mpsc */
    } /* namespace lockfree */
} /* namespace qb */

#endif //QB_LOCKFREE_MPSC_H
