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

#ifndef QB_PIPE_H
# define QB_PIPE_H
# include <memory>
# include <cstring>
# include <qb/utility/nocopy.h>
# include <qb/utility/prefix.h>
# include <qb/utility/branch_hints.h>

namespace qb {
    namespace allocator {

        template <typename T, typename U>
        constexpr auto getItemSize() {
            return sizeof(T) / sizeof(U) + static_cast<bool>(sizeof(T) % sizeof(U));
        }

        template <typename T, std::size_t _SIZE = 4096>
        class pipe : nocopy, std::allocator<T> {
            using base_type = std::allocator<T>;
        protected:
            std::size_t _begin;
            std::size_t _end;
            bool flag_front;
            char __padding2__[QB_LOCKFREE_CACHELINE_BYTES - (2 *sizeof(std::size_t) + sizeof(bool))];
            std::size_t _capacity;
            std::size_t _factor;
            T *_data;

        public:
            pipe() :  _begin(0)
                    , _end(0)
                    , flag_front(false)
                    , _capacity(_SIZE)
                    , _factor(1)
                    , _data(base_type::allocate(_SIZE)) {
            }

            ~pipe() {
                base_type::deallocate(_data, _capacity);
            }

            inline std::size_t capacity() const {
                return _capacity;
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
                flag_front = false;
            }

            inline void free(std::size_t const size) {
                if (flag_front)
                    _begin += size;
                else
                    _end -= size;
            }

            inline auto *allocate_back(std::size_t const size) {
                if (likely(_end + size < _capacity)) {
                    const auto save_index = _end;
                    _end += size;
                    return _data + save_index;
                }
                const auto nb_item = _end - _begin;
                const auto half = _capacity / 2;
                if (_begin > half && size < half) {
                    reorder();
                    _end += size;
                    return _data + nb_item;
                } else {
                    std::size_t new_capacity;
                    do {
                        _factor <<= 1;
                        new_capacity = _factor * _SIZE;
                    } while (new_capacity - nb_item < size);

                    const auto new_data = base_type::allocate(new_capacity);
                    std::memcpy(new_data, _data + _begin, nb_item * sizeof(T));
                    base_type::deallocate(_data, _capacity);

                    _begin = 0;
                    _end = nb_item + size;
                    _capacity = new_capacity;
                    _data = new_data;
                }
                return _data + nb_item;
            }

            template <typename U, typename ..._Init>
            inline U &allocate_back(_Init &&...init) {
                constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
                return *(new (reinterpret_cast<U *>(allocate_back(BUCKET_SIZE))) U(std::forward<_Init>(init)...));
            }

            template <typename U, typename ..._Init>
            inline U &allocate_size(std::size_t const size, _Init &&...init) {
                constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
                return *(new (reinterpret_cast<U *>(allocate_back(size + BUCKET_SIZE))) U(std::forward<_Init>(init)...));
            }

            inline auto allocate(std::size_t const size) {
                if (_begin - (size + 1) < _end) {
                    _begin -= size;
                    flag_front = true;
                    return _data + _begin;
                }
                flag_front = false;
                return allocate_back(size);
            }

            template <typename U, typename ..._Init>
            inline U &allocate(_Init &&...init) {
                constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
                return *(new (reinterpret_cast<U *>(allocate(BUCKET_SIZE))) U(std::forward<_Init>(init)...));
            }

            template <typename U>
            inline U &recycle_back(U const &data) {
                constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
                return *reinterpret_cast<U *>(std::memcpy(allocate_back(BUCKET_SIZE)
                        , &data, sizeof(U)));
            }

            template <typename U>
            inline U &recycle_back(U const &data, std::size_t const size) {
                return *reinterpret_cast<U *>(std::memcpy(allocate_back(size)
                        , &data, size * sizeof(T)));
            }

            template <typename U>
            inline U &recycle(U const &data) {
                constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
                return *reinterpret_cast<U *>(std::memcpy(allocate(BUCKET_SIZE)
                        , &data, sizeof(U)));
            }

            template <typename U>
            inline U &recycle(U const &data, std::size_t const size) {
                return *reinterpret_cast<U *>(std::memcpy(allocate(size)
                        , &data, size * sizeof(T)));
            }

            inline void reorder() {
                const auto nb_item = _end - _begin;
                //std::cout << "Start reorder " << _begin << ":" << _end << "|" << nb_item;
                std::memmove(_data, _data + _begin, nb_item * sizeof(T));
                _begin = 0;
                _end = nb_item;
                //std::cout << "End reorder " << _begin << ":" << _end << "|" << _end - _begin;
            }

        };

    }
}

#endif //QB_PIPE_H
