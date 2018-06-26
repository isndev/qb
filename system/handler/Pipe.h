
#ifndef CUBE_PIPE_H
# define CUBE_PIPE_H
# include <memory>
# include <cstring>
# include "utils/nocopy.h"
# include "system/Types.h"

namespace cube {

    template <typename T, std::size_t _SIZE = 4096>
    class pipe_allocator : nocopy, std::allocator<T> {
        using base_type = std::allocator<T>;
    protected:
        std::size_t _begin;
        std::size_t _end;
        char __padding2__[CUBE_LOCKFREE_CACHELINE_BYTES - (2 *sizeof(std::size_t))];
        std::size_t _capacity;
        std::size_t _factor;
        T *_data;

    public:
        pipe_allocator() :  _begin(0)
                , _end(0)
                , _capacity(_SIZE)
                , _factor(1)
                , _data(base_type::allocate(_SIZE)) {
        }

        ~pipe_allocator() {
            base_type::deallocate(_data, _capacity);
        }

        inline const auto data() const {
            return _data;
        }

        inline std::size_t begin() const {
            return _begin;
        }

        inline std::size_t end() const {
            return _end;
        }

        inline void free_front(std::size_t const size) {
            _begin += size;
        }

        inline void free_back(std::size_t const size) {
            _end -= size;
        }

        inline void reset(std::size_t const begin) {
            if (begin != _end)
                _begin = begin;
            else {
                _begin = 0;
                _end = 0;
            }
        }

        inline void reset() {
            _begin = 0;
            _end = 0;
        }

        inline void free(std::size_t const size) {
            if (_begin - size >= _begin)
                _end -= size;
            else
                _begin += size;
        }

        inline auto *allocate_back(std::size_t const size) {
            if (likely(_end + size < _capacity)) {
                const auto save_index = _end;
                _end += size;
                return _data + save_index;
            }
            _factor <<= 1;
            const auto nb_item = _end - _begin;
            const auto new_capacity = _factor * _SIZE;
            const auto new_data = base_type::allocate(new_capacity);
            std::memcpy(new_data, _data + _begin, nb_item * sizeof(T));
            base_type::deallocate(_data, _capacity);

            _begin = 0;
            _end = nb_item + size;
            _capacity = new_capacity;
            _data = new_data;
            return _data + nb_item;
        }

        template <typename U, typename ..._Init>
        inline U &allocate_back(_Init &&...init) {
            constexpr std::size_t BUCKET_SIZE = (sizeof(U) / sizeof(T));
            return *(new (reinterpret_cast<U *>(allocate_back(BUCKET_SIZE))) U(std::forward<_Init>(init)...));
        }

        inline auto allocate(uint16_t const size) {
            if (_begin - size < _end) {
                _begin -= size;
                return _data + _begin;
            }

            return allocate_back(size);
        }

        template <typename U, typename ..._Init>
        inline U &allocate(_Init &&...init) {
            constexpr std::size_t BUCKET_SIZE = (sizeof(U) / sizeof(T));
            return *(new (reinterpret_cast<U *>(allocate(sizeof(U) / sizeof(T)))) U(std::forward<_Init>(init)...));
        }

        template <typename U>
        inline U &recycle_back(U const &data) {
            return *reinterpret_cast<U *>(std::memcpy(allocate_back(sizeof(U) / sizeof(T))
                    , &data, sizeof(U)));
        }

        template <typename U>
        U &recycle(U const &data) {
            return *reinterpret_cast<U *>(std::memcpy(allocate(sizeof(U) / sizeof(T))
                    , &data, sizeof(U)));
        }

        template <typename U>
        U &recycle(U const &data, std::size_t const size) {
            return *reinterpret_cast<U *>(std::memcpy(allocate(size)
                    , &data, size * sizeof(T)));
        }
    };

}

#endif //CUBE_PIPE_H
