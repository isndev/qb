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

#ifndef QB_LOCKFREE_RINGBUFFER_H
# define QB_LOCKFREE_RINGBUFFER_H
# include <cstdint>
# include <cstring>
# include <array>
# include <algorithm>
# include <thread>
# include <atomic>
# include <memory>
# include <qb/utility/branch_hints.h>
# include <qb/utility/prefix.h>
# include <qb/utility/nocopy.h>

namespace qb {
    namespace lockfree {
        namespace spsc {
            namespace internal {
                template<typename T>
                class ringbuffer
                        : public nocopy {
                    typedef std::size_t size_t;
                    constexpr static const int padding_size = QB_LOCKFREE_CACHELINE_BYTES - sizeof(size_t);
                    std::atomic<size_t> write_index_;
                    char padding1[padding_size]; /* force read_index and write_index to different cache lines */
                    std::atomic<size_t> read_index_;

                protected:
                    ringbuffer(void) :
                            write_index_(0), read_index_(0)
                    {}

                    static size_t next_index(size_t arg, size_t const max_size)
                    {
                        size_t ret = arg + 1;
                        while (unlikely(ret >= max_size))
                            ret -= max_size;
                        return ret;
                    }

                    static size_t read_available(size_t write_index, size_t read_index, size_t const max_size)
                    {
                        if (write_index >= read_index)
                            return write_index - read_index;

                        const size_t ret = write_index + max_size - read_index;
                        return ret;
                    }

                    static size_t write_available(size_t write_index, size_t read_index, size_t const max_size)
                    {
                        size_t ret = read_index - write_index - 1;
                        if (write_index >= read_index)
                            ret += max_size;
                        return ret;
                    }

                    size_t read_available(size_t const max_size) const
                    {
                        size_t write_index = write_index_.load(std::memory_order_acquire);
                        const size_t read_index = read_index_.load(std::memory_order_relaxed);
                        return read_available(write_index, read_index, max_size);
                    }

                    size_t write_available(size_t const max_size) const
                    {
                        size_t write_index = write_index_.load(std::memory_order_relaxed);
                        const size_t read_index = read_index_.load(std::memory_order_acquire);
                        return write_available(write_index, read_index, max_size);
                    }

                    bool enqueue(T const & t, T * buffer, size_t const max_size)
                    {
                        const size_t write_index = write_index_.load(std::memory_order_relaxed);  // only written from enqueue thread
                        const size_t next = next_index(write_index, max_size);

                        if (next == read_index_.load(std::memory_order_acquire))
                            return false; /* ringbuffer is full */

                        new (buffer + write_index) T(t); // copy-construct

                        write_index_.store(next, std::memory_order_release);

                        return true;
                    }

                    template <bool _All>
                    size_t enqueue(const T * input_buffer, size_t input_count, T * internal_buffer, size_t const max_size)
                    {
                        const size_t write_index = write_index_.load(std::memory_order_relaxed);  // only written from push thread
                        const size_t read_index = read_index_.load(std::memory_order_acquire);
                        const size_t avail = write_available(write_index, read_index, max_size);

                        if constexpr (_All) {
                            if (avail < input_count)
                                return 0;
                        } else {
                            if (!avail)
                                return 0;
                            input_count = (std::min)(input_count, avail);
                        }

                        size_t new_write_index = write_index + input_count;

                        if (write_index + input_count > max_size) {
                            /* copy data in two sections */
                            size_t count0 = max_size - write_index;

                            std::uninitialized_copy(input_buffer, input_buffer + count0, internal_buffer + write_index);
                            std::uninitialized_copy(input_buffer + count0, input_buffer + input_count, internal_buffer);
                            new_write_index -= max_size;
                        }
                        else {
                            std::uninitialized_copy(input_buffer, input_buffer + input_count,
                                                    internal_buffer + write_index);

                            if (new_write_index == max_size)
                                new_write_index = 0;
                        }

                        write_index_.store(new_write_index, std::memory_order_release);
                        return input_count;
                    }


                    size_t dequeue(T * output_buffer, size_t output_count, T * internal_buffer, size_t const max_size)
                    {
                        const size_t write_index = write_index_.load(std::memory_order_acquire);
                        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread

                        const size_t avail = read_available(write_index, read_index, max_size);

                        if (avail == 0)
                            return 0;

                        output_count = (std::min)(output_count, avail);

                        size_t new_read_index = read_index + output_count;

                        if (read_index + output_count > max_size) {
                            /* copy data in two sections */
                            const size_t count0 = max_size - read_index;
                            const size_t count1 = output_count - count0;

                            std::memcpy(output_buffer, internal_buffer + read_index, count0 * sizeof(T));
                            std::memcpy(output_buffer + count0, internal_buffer, count1 * sizeof(T));

                            new_read_index -= max_size;
                        }
                        else {
                            std::memcpy(output_buffer, internal_buffer + read_index, output_count * sizeof(T));

                            if (new_read_index == max_size)
                                new_read_index = 0;
                        }

                        read_index_.store(new_read_index, std::memory_order_release);
                        return output_count;
                    }

                    const T& front(const T * internal_buffer) const
                    {
                        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
                        return *(internal_buffer + read_index);
                    }

                    T& front(T * internal_buffer)
                    {
                        const size_t read_index = read_index_.load(std::memory_order_relaxed); // only written from pop thread
                        return *(internal_buffer + read_index);
                    }

                public:
                    bool empty(void)
                    {
                        return empty(write_index_.load(std::memory_order_relaxed), read_index_.load(std::memory_order_relaxed));
                    }

                private:
                    bool empty(size_t write_index, size_t read_index)
                    {
                        return write_index == read_index;
                    }
                };

            } /* namespace internal */

            template<typename T, size_t _MaxSize>
            class ringbuffer
                    : public internal::ringbuffer<T> {
                typedef std::size_t size_t;
                constexpr static size_t max_size = _MaxSize + 1;
                std::array<T, max_size> array_;

            public:
                inline bool enqueue(T const &t) noexcept {
                    return internal::ringbuffer<T>::enqueue(t, array_.data(), max_size);
                }

                inline bool dequeue(T *ret) noexcept {
                    return internal::ringbuffer<T>::dequeue(ret, array_.data(), max_size);
                }

                template <bool _All = true>
                inline size_t enqueue(T const *t, size_t size) noexcept {
                    return internal::ringbuffer<T>::template enqueue<_All>(t, size, array_.data(), max_size);
                }

                inline size_t dequeue(T *ret, size_t size) noexcept {
                    return internal::ringbuffer<T>::dequeue(ret, size, array_.data(), max_size);
                }

                template <typename Func>
                inline size_t dequeue(Func const &func, T *ret, size_t size) noexcept {
                    const size_t nb_consume = internal::ringbuffer<T>::dequeue(ret, size, array_.data(), max_size);
                    if (nb_consume)
                        func(ret, nb_consume);
                    return nb_consume;
                }
            };

            template<typename T>
            class ringbuffer<T, 0>
                    : public internal::ringbuffer<T> {
                typedef std::size_t size_t;
                const size_t max_size_;
                std::unique_ptr<T> array_;

            public:
                //! Constructs a ringbuffer for max_size elements
                explicit ringbuffer(size_t const max_size)
                        : max_size_(max_size + 1), array_(new T[max_size + 1]) {}

                inline bool enqueue(T const &t) noexcept {
                    return internal::ringbuffer<T>::enqueue(t, array_.get(), max_size_);
                }

                inline bool dequeue(T *ret) noexcept {
                    return internal::ringbuffer<T>::dequeue(ret, array_.get(), max_size_);
                }

                template <bool _All = true>
                inline size_t enqueue(T const *t, size_t size) noexcept {
                    return internal::ringbuffer<T>::template enqueue<_All>(t, size, array_.get(), max_size_);
                }

                inline size_t dequeue(T *ret, size_t size) noexcept {
                    return internal::ringbuffer<T>::dequeue(ret, size, array_.get(), max_size_);
                }

                template <typename Func>
                inline size_t dequeue(Func const &func, T *ret, size_t size) noexcept {
                    const size_t nb_consume = internal::ringbuffer<T>::dequeue(ret, size, array_.get(), max_size_);
                    if (nb_consume)
                        func(ret, nb_consume);
                    return nb_consume;
                }
            };

        } /* namespace spsc */
    } /* namespace lockfree */
} /* namespace qb */


#endif /* QB_LOCKFREE_RINGBUFFER_H */
