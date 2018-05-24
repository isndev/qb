
#ifndef CUBE_MPSC_H
#define CUBE_MPSC_H
# include <mutex>
# include "spsc.h"
# include "spinlock.h"

namespace cube {
    namespace lockfree {
        namespace mpsc {
//            namespace internal {
//
//                template<typename T, typename _Size = uint64_t>
//                class ringbuffer
//                        : public nocopy {
//                    struct Producer
//                    {
//                        SpinLock lock;
//                        spsc::ringbuffer<> ;
//
//                        Producer(size_t capacity) : buffer(capacity) {}
//                    };
//
//                    size_t _capacity;
//                    size_t _concurrency;
//                    std::vector<std::shared_ptr<Producer>> _producers;
//                    size_t _consumer;
//                protected:
//
//                public:
//                    ~mpsc_ringbuffer() = default;
//                };
//
//            } /* namespace internal */

            template<typename T, size_t nb_producer, size_t max_size, typename _Size = uint64_t>
            class ringbuffer
                    : public nocopy {
                typedef _Size size_t;
                struct Producer
                {
                    constexpr static const int padding_size = CUBE_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
                    SpinLock lock;
                    char padding1[padding_size];
                    spsc::ringbuffer<T, max_size, size_t> _ringbuffer;
                };

                std::array<Producer, nb_producer> _producers;

            public:
                template <size_t _Index>
                bool enqueue(T const &t) {
                    const size_t index = _Index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t);
                }

                template <size_t _Index>
                size_t enqueue(T const *t, size_t const size) {
                    const size_t index = _Index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t, size);
                }

                bool enqueue(size_t const _index, T const &t) {
                    const size_t index = _index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index].enqueue(t);
                }

                size_t enqueue(size_t const _index, T const *t, size_t const size) {
                    const size_t index = _index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t, size);
                }

                size_t enqueue(T const &t) {
                    const size_t index = Timestamp::rdts() % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t);
                }

                size_t enqueue(T const *t, size_t const size) {
                    const size_t index = Timestamp::rdts() % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index].lock);
                    return _producers[index]._ringbuffer.enqueue(t, size);
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

//            template<typename T>
//            class ringbuffer<T, 0> :
//                    public internal::ringbuffer<T> {
//                typedef std::size_t size_t;
//                size_t max_size_;
//                T *array_;
//
//            public:
//                //! Constructs a ringbuffer for max_size elements
//                explicit ringbuffer(size_t max_size) :
//                        max_size_(max_size), array_(new T[max_size]) {}
//
//                inline bool enqueue(T const &t) {
//                    return internal::ringbuffer<T>::enqueue(t, array_.get(), max_size_);
//                }
//
//                inline bool dequeue(T *ret) {
//                    return internal::ringbuffer<T>::dequeue(ret, array_.get(), max_size_);
//                }
//
//                inline size_t enqueue(T const *t, size_t size) {
//                    return internal::ringbuffer<T>::enqueue(t, size, array_.get(), max_size_);
//                }
//
//                inline size_t dequeue(T *ret, size_t size) {
//                    return internal::ringbuffer<T>::dequeue(ret, size, array_.get(), max_size_);
//                }
//            };

        } /* namespace mpsc */
    } /* namespace lockfree */
} /* namespace cube */

#endif //CUBE_MPSC_H
