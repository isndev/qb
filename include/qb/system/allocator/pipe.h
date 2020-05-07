/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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
#define QB_PIPE_H
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <qb/utility/branch_hints.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/prefix.h>
#include <qb/string.h>
#include <string_view>
#include <vector>

namespace qb::allocator {

template <typename T, typename U>
constexpr auto
getItemSize() {
    return sizeof(T) / sizeof(U) + static_cast<bool>(sizeof(T) % sizeof(U));
}

template <typename T>
class base_pipe
    : nocopy
    , std::allocator<T> {
    using base_type = std::allocator<T>;
    constexpr static const std::size_t _SIZE = 4096;

protected:
    std::size_t _begin;
    std::size_t _end;
    bool _flag_front;
    std::size_t _capacity;
    std::size_t _factor;
    T *_data;

public:
    base_pipe()
        : _begin(0)
        , _end(0)
        , _flag_front(false)
        , _capacity(_SIZE)
        , _factor(1)
        , _data(base_type::allocate(_SIZE)) {}

    ~base_pipe() {
        base_type::deallocate(_data, _capacity);
    }

    [[nodiscard]] inline std::size_t
    capacity() const noexcept {
        return _capacity;
    }

    [[nodiscard]] inline T *
    data() const noexcept {
        return _data;
    }

    [[nodiscard]] inline T *
    begin() const noexcept {
        return _data + _begin;
    }

    [[nodiscard]] inline T *
    end() const noexcept {
        return _data + _end;
    }

    [[nodiscard]] inline const T *
    cbegin() const noexcept {
        return _data + _begin;
    }

    [[nodiscard]] inline const T *
    cend() const noexcept {
        return _data + _end;
    }

    [[nodiscard]] inline std::size_t
    size() const noexcept {
        return _end - _begin;
    }

    inline void
    free_front(std::size_t const size) noexcept {
        _begin += size;
    }

    inline void
    free_back(std::size_t const size) noexcept {
        _end -= size;
    }

    inline void
    reset(std::size_t const begin) noexcept {
        if (begin != _end)
            _begin = begin;
        else {
            _begin = 0;
            _end = 0;
        }
    }

    inline void
    reset() noexcept {
        _begin = 0;
        _end = 0;
        _flag_front = false;
    }

    inline void
    free(std::size_t const size) noexcept {
        if (_flag_front)
            _begin += size;
        else
            _end -= size;
    }

    inline auto *
    allocate_back(std::size_t const size) {
        if (likely(_end + size <= _capacity)) {
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
                _factor <<= 1u;
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

    template <typename U, typename... _Init>
    inline U &
    allocate_back(_Init &&... init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate_back(BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    template <typename U, typename... _Init>
    inline U &
    allocate_size(std::size_t const size, _Init &&... init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate_back(size + BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    inline auto
    allocate(std::size_t const size) {
        if (_begin - (size + 1) < _end) {
            _begin -= size;
            _flag_front = true;
            return _data + _begin;
        }
        _flag_front = false;
        return allocate_back(size);
    }

    template <typename U, typename... _Init>
    inline U &
    allocate(_Init &&... init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate(BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    template <typename U>
    inline U &
    recycle_back(U const &data) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *reinterpret_cast<U *>(
            std::memcpy(allocate_back(BUCKET_SIZE), &data, sizeof(U)));
    }

    template <typename U>
    inline U &
    recycle_back(U const &data, std::size_t const size) {
        return *reinterpret_cast<U *>(
            std::memcpy(allocate_back(size), &data, size * sizeof(T)));
    }

    template <typename U>
    inline U &
    recycle(U const &data) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *reinterpret_cast<U *>(
            std::memcpy(allocate(BUCKET_SIZE), &data, sizeof(U)));
    }

    template <typename U>
    inline U &
    recycle(U const &data, std::size_t const size) {
        return *reinterpret_cast<U *>(
            std::memcpy(allocate(size), &data, size * sizeof(T)));
    }

    inline void
    reorder() noexcept {
        if (!_begin)
            return;
        const auto nb_item = _end - _begin;
        // std::cout << "Start reorder " << _begin << ":" << _end << "|" << nb_item;
        std::memmove(_data, _data + _begin, nb_item * sizeof(T));
        _begin = 0;
        _end = nb_item;
        // std::cout << "End reorder " << _begin << ":" << _end << "|" << _end - _begin;
    }

    inline void
    flush() const noexcept {}

    void
    reserve(std::size_t const size) {
        allocate_back(size);
        free_back(size);
    }
};

template <typename T>
class QB_LOCKFREE_CACHELINE_ALIGNMENT pipe : public base_pipe<T> {
public:
    inline void
    swap(pipe &rhs) noexcept {
        std::swap(*reinterpret_cast<CacheLine *>(this),
                  *reinterpret_cast<CacheLine *>(&rhs));
    }

    template <typename _It>
    pipe &
    put(_It begin, _It const &end) {
        auto out = allocate_back(end - begin);
        while (begin != end) {
            *out = *begin;
            ++out;
            ++begin;
        }
        return *this;
    }

    pipe &
    put(T const *data, std::size_t const size) {
        memcpy(this->allocate_back(size), data, size * sizeof(T));
        return *this;
    }

    template <typename U>
    pipe &
    operator<<(U &&rhs) noexcept {
        this->template allocate_back<U>(std::forward<U>(rhs));
        return *this;
    }
};

template <>
class pipe<char> : public base_pipe<char> {
public:
    template <typename U>
    pipe &
    put(const U &rhs) {
        return put(std::to_string(rhs));
    }

    template <std::size_t _Size>
    pipe &
    put(const char (&str)[_Size]) {
        memcpy(allocate_back(_Size - 1), str, _Size - 1);
        return *this;
    }

    template <std::size_t _Size>
    pipe &
    put(const qb::string<_Size> &str) {
        memcpy(allocate_back(str.size()), str.c_str(), str.size());
        return *this;
    }

    template <typename T>
    pipe &
    put(std::vector<T> const &vec) {
        memcpy(allocate_back(vec.size()), reinterpret_cast<const char *>(vec.data()),
               vec.size());
        return *this;
    }

    template <typename T, std::size_t _Size>
    pipe &
    put(std::array<T, _Size> const &arr) {
        memcpy(allocate_back(arr.size()), reinterpret_cast<const char *>(arr.data()),
               arr.size());
        return *this;
    }

    template <typename _It>
    pipe &
    put(_It begin, _It const &end) {
        auto out = allocate_back(end - begin);
        while (begin != end) {
            *out = *begin;
            ++out;
            ++begin;
        }
        return *this;
    }

    template <typename U>
    pipe &
    operator<<(const U &rhs) noexcept {
        return put(rhs);
    }

    pipe &put(char const *data, std::size_t size) noexcept;

    pipe &write(const char *data, std::size_t size) noexcept;

    std::string str() const noexcept;

    std::string_view view() const noexcept;
};

template <>
pipe<char> &pipe<char>::put<char>(const char &c);

template <>
pipe<char> &pipe<char>::put<unsigned char>(const unsigned char &c);

template <>
pipe<char> &pipe<char>::put<const char *>(const char *const &c);

template <>
pipe<char> &pipe<char>::put<std::string>(std::string const &str);

template <>
pipe<char> &pipe<char>::put<std::string_view>(std::string_view const &str);

template <>
pipe<char> &pipe<char>::put<pipe<char>>(pipe<char> const &rhs);

} // namespace qb::allocator

template <typename stream, typename = std::enable_if_t<!std::is_same_v<stream, qb::allocator::pipe<char>>>>
stream &
operator<<(stream &os, qb::allocator::pipe<char> const &p) {
    os << std::string_view(p.begin(), p.size());
    return os;
}

#endif // QB_PIPE_H