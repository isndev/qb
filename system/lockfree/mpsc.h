
#ifndef CUBE_MPSC_H
#define CUBE_MPSC_H
# include "spsc.h"
# include "spinlock.h"
# include <mutex>

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
                struct Producer
                {
                    constexpr static const int padding_size = CUBE_LOCKFREE_CACHELINE_BYTES - sizeof(SpinLock);
                    SpinLock lock;
                    char padding1[padding_size];
                    spsc::ringbuffer<T, max_size, _Size> _ringbuffer;
                };

                std::array<Producer, nb_producer> _producers;

//                size_t _capacity;
//                size_t _concurrency;
//                std::vector<std::shared_ptr<Producer>> _producers;
//                size_t _consumer;

            public:
                template <size_t _Index>
                bool enqueue(T const &t) {
                    const size_t index = _Index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index]->lock);
                    return _producers[index]._ringbuffer.enqueue(t);
                }

                bool enqueue(size_t const _index, T const &t) {
                    const size_t index = _index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index]->lock);
                    return _producers[index].enqueue(t);
                }

                template <size_t _Index>
                size_t enqueue(T const *t, size_t const size) {
                    const size_t index = _Index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index]->lock);
                    return _producers[index]._ringbuffer.enqueue(t, size);
                }

                size_t enqueue(size_t const _index, T const *t, size_t const size) {
                    const size_t index = _index % nb_producer;
                    std::lock_guard<SpinLock> lock(_producers[index]->lock);
                    return _producers[index]._ringbuffer.enqueue(t, size);
                }

                size_t dequeue(T *ret, size_t size) {
                    size_t tmp_size = size;
                    for (auto &producer : _producers) {

                        if (size)

                    }

                    return tmp_size;
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

class mpsc_ringbuffer
{
public:

    explicit mpsc_ringbuffer(size_t capacity, size_t concurrency = std::thread::hardware_concurrency())
            : _capacity(capacity - 1), _concurrency(concurrency), _consumer(0)
    {
        // Initialize producers' ring buffer
        for (size_t i = 0; i < concurrency; ++i)
            _producers.push_back(std::make_shared<Producer>(capacity));
    }
    mpsc_ringbuffer(const mpsc_ringbuffer&) = delete;
    mpsc_ringbuffer(mpsc_ringbuffer&&) noexcept = default;
    ~mpsc_ringbuffer() = default;

    mpsc_ringbuffer& operator=(const mpsc_ringbuffer&) = delete;
    mpsc_ringbuffer& operator=(mpsc_ringbuffer&&) noexcept = default;

    bool empty() const noexcept { return (size() == 0); }
    size_t capacity() const noexcept { return _capacity; }
    size_t concurrency() const noexcept { return _concurrency; }
    size_t size() const noexcept
    {
        size_t size = 0;
        for (auto& producer : _producers)
            size += producer->buffer.size();
        return size;
    }


    bool produce(const void* chunk, size_t size){
        // Get producer index for the current thread based on RDTS value
        size_t index = Timestamp::rdts() % _concurrency;

        // Lock the chosen producer using its spin-lock
        std::lock_guard<SpinLock> lock(_producers[index]->lock);

        // Enqueue the item into the producer's ring buffer
        return _producers[index]->buffer.produce(chunk, size);
    }


    bool consume(void* chunk, size_t& size){
        // Try to dequeue one item from the one of producer's ring buffers
        for (size_t i = 0; i < _concurrency; ++i)
        {
            size_t temp = size;
            if (_producers[_consumer++ % _concurrency]->buffer.consume(chunk, temp))
            {
                size = temp;
                return true;
            }
        }

        size = 0;
        return false;
    }

private:

};

#endif //CUBE_MPSC_H
