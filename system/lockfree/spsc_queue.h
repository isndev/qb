
#ifndef CUBE_LOCKFREE_RINGBUFFER_H
# define CUBE_LOCKFREE_RINGBUFFER_H
# include <cstdint>
# include <array>
# include <algorithm>
# include <thread>
# include <atomic>

# include "utils/nocopy.h"
# include "utils/branch_hints.h"
# include "utils/prefix.h"

namespace cube {
    namespace lockfree {

        namespace detail {

            template<typename T, typename _Size = uint64_t>
            class ringbuffer_internal :
                    nocopy {
                typedef _Size size_t;
                static const int padding_size = CUBE_LOCKFREE_CACHELINE_BYTES - sizeof(size_t);
                std::atomic<size_t> write_index_;
                char padding1[padding_size]; /* force read_index and write_index to different cache lines */
                std::atomic<size_t> read_index_;

            protected:
                ringbuffer_internal(void) :
                        write_index_(0), read_index_(0) {}

                static size_t next_index(size_t arg, size_t max_size) {
                    size_t ret = arg + 1;
                    while (unlikely(ret >= max_size))
                        ret -= max_size;
                    return ret;
                }

                static size_t read_available(size_t write_index, size_t read_index, size_t max_size) {
                    if (write_index >= read_index)
                        return write_index - read_index;

                    size_t ret = write_index + max_size - read_index;
                    return ret;
                }

                static size_t write_available(size_t write_index, size_t read_index, size_t max_size) {
                    size_t ret = read_index - write_index - 1;
                    if (write_index >= read_index)
                        ret += max_size;
                    return ret;
                }

                bool enqueue(T const &t, T *buffer, size_t max_size) {
                    size_t write_index = write_index_.load(std::memory_order_acquire);
                    size_t next = next_index(write_index, max_size);

                    if (next == read_index_.load(std::memory_order_acquire))
                        return false; /* ringbuffer is full */

                    buffer[write_index] = t;

                    write_index_.store(next, std::memory_order_release);

                    return true;
                }

                bool dequeue(T *ret, T *buffer, size_t max_size) {
                    size_t write_index = write_index_.load(std::memory_order_acquire);
                    size_t read_index = read_index_.load(std::memory_order_acquire);
                    if (empty(write_index, read_index))
                        return false;

                    *ret = buffer[read_index];
                    size_t next = next_index(read_index, max_size);
                    read_index_.store(next, std::memory_order_release);
                    return true;
                }

                size_t enqueue(const T *input_buffer, size_t input_count, T *internal_buffer, size_t max_size) {
                    size_t write_index = write_index_.load(std::memory_order_acquire);
                    const size_t read_index = read_index_.load(std::memory_order_acquire);
                    const size_t avail = write_available(write_index, read_index, max_size);

                    if (avail == 0)
                        return 0;

                    input_count = (std::min)(input_count, avail);

                    size_t new_write_index = write_index + input_count;

                    if (write_index + input_count > max_size) {
                        /* copy data in two sections */
                        size_t count0 = max_size - write_index;

                        std::uninitialized_copy(input_buffer, input_buffer + count0, internal_buffer + write_index);
                        std::uninitialized_copy(input_buffer + count0, input_buffer + input_count, internal_buffer);
                        new_write_index -= max_size;
                    } else {
                        std::uninitialized_copy(input_buffer, input_buffer + input_count,
                                                internal_buffer + write_index);

                        if (new_write_index == max_size)
                            new_write_index = 0;
                    }

                    write_index_.store(new_write_index, std::memory_order_release);
                    return input_count;
                }

                size_t dequeue(T *output_buffer, size_t output_count, const T *internal_buffer, size_t max_size) {
                    const size_t write_index = write_index_.load(std::memory_order_acquire);
                    size_t read_index = read_index_.load(std::memory_order_acquire);

                    const size_t avail = read_available(write_index, read_index, max_size);

                    if (avail == 0)
                        return 0;

                    output_count = (std::min)(output_count, avail);

                    size_t new_read_index = read_index + output_count;

                    if (read_index + output_count > max_size) {
                        /* copy data in two sections */
                        size_t count0 = max_size - read_index;
                        size_t count1 = output_count - count0;

                        std::copy(internal_buffer + read_index, internal_buffer + max_size, output_buffer);
                        std::copy(internal_buffer, internal_buffer + count1, output_buffer + count0);

                        new_read_index -= max_size;
                    } else {
                        std::copy(internal_buffer + read_index, internal_buffer + read_index + output_count,
                                  output_buffer);
                        if (new_read_index == max_size)
                            new_read_index = 0;
                    }

                    read_index_.store(new_read_index, std::memory_order_release);
                    return output_count;
                }


            public:
                /** reset the ringbuffer
                 *
                 * \warning Not thread-safe, use for debugging purposes only
                 * */
                void reset(void) {
                    write_index_.store(0, std::memory_order_relaxed);
                    read_index_.store(0, std::memory_order_release);
                }

                /**
                 * \return true, if ringbuffer is empty.
                 *
                 * \warning Not thread-safe, use for debugging purposes only
                 * */
                bool empty(void) {
                    return empty(write_index_.load(std::memory_order_relaxed),
                                 read_index_.load(std::memory_order_relaxed));
                }

                //! \copydoc cube::lockfree::fifo::is_lock_free
                bool is_lock_free(void) const {
                    return write_index_.is_lock_free() && read_index_.is_lock_free();
                }

            private:
                bool empty(size_t write_index, size_t read_index) {
                    return write_index == read_index;
                }
            };

        } /* namespace detail */

        template<typename T, size_t max_size, typename _Size = uint64_t>
        class ringbuffer :
                public detail::ringbuffer_internal<T> {
            typedef _Size size_t;
            std::array<T, max_size> array_;

        public:
            /** Enqueues object t to the ringbuffer. Enqueueing may fail, if the ringbuffer is full.
             *
             * \return true, if the enqueue operation is successful.
             *
             * */
            bool enqueue(T const &t) {
                return detail::ringbuffer_internal<T>::enqueue(t, array_.data(), max_size);
            }

            /** Dequeue object from ringbuffer.
             *
             * If dequeue operation is successful, object is written to memory location denoted by ret.
             *
             * \return true, if the dequeue operation is successful, false if ringbuffer was empty.
             *
             */
            bool dequeue(T *ret) {
                return detail::ringbuffer_internal<T>::dequeue(ret, array_.data(), max_size);
            }

            /** Enqueues size objects from the array t to the ringbuffer.
             *
             *  Enqueueing may fail, if the ringbuffer is full.
             *
             * \return number of enqueued items
             *
             * \note Thread-safe and non-blocking
             */
            size_t enqueue(T const *t, size_t size) {
                return detail::ringbuffer_internal<T>::enqueue(t, size, array_.data(), max_size);
            }

            /** Dequeue a maximum of size objects from ringbuffer.
             *
             * If dequeue operation is successful, object is written to memory location denoted by ret.
             *
             * \return number of dequeued items
             *
             * */
            size_t dequeue(T *ret, size_t size) {
                return detail::ringbuffer_internal<T>::dequeue(ret, size, array_.data(), max_size);
            }
        };

        template<typename T>
        class ringbuffer<T, 0> :
                public detail::ringbuffer_internal<T> {
            typedef std::size_t size_t;
            size_t max_size_;
            T *array_;

        public:
            //! Constructs a ringbuffer for max_size elements
            explicit ringbuffer(size_t max_size) :
                    max_size_(max_size), array_(new T[max_size]) {}

            /** Enqueues object t to the ringbuffer. Enqueueing may fail, if the ringbuffer is full.
             *
             * \return true, if the enqueue operation is successful.
             *
             * */
            bool enqueue(T const &t) {
                return detail::ringbuffer_internal<T>::enqueue(t, array_.get(), max_size_);
            }

            /** Dequeue object from ringbuffer.
             *
             * If dequeue operation is successful, object is written to memory location denoted by ret.
             *
             * \return true, if the dequeue operation is successful, false if ringbuffer was empty.
             *
             */
            bool dequeue(T *ret) {
                return detail::ringbuffer_internal<T>::dequeue(ret, array_.get(), max_size_);
            }

            /** Enqueues size objects from the array t to the ringbuffer.
             *
             *  Enqueueing may fail, if the ringbuffer is full.
             *
             * \Returns number of enqueued items
             *
             * \note Thread-safe and non-blocking
             */
            size_t enqueue(T const *t, size_t size) {
                return detail::ringbuffer_internal<T>::enqueue(t, size, array_.get(), max_size_);
            }

            /** Dequeue a maximum of size objects from ringbuffer.
             *
             * If dequeue operation is successful, object is written to memory location denoted by ret.
             *
             * \return number of dequeued items
             *
             * */
            size_t dequeue(T *ret, size_t size) {
                return detail::ringbuffer_internal<T>::dequeue(ret, size, array_.get(), max_size_);
            }
        };


    } /* namespace lockfree */
} /* namespace cube */


#endif /* CUBE_LOCKFREE_RINGBUFFER_H */
