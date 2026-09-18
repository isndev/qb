/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file qb/system/container/growable_ring.h
 * @brief A growable single-thread FIFO ring: one allocation per doubling, none per element.
 * @details The coroutine layer queued chunks and parked waiters in `std::deque`s. libstdc++ packs
 *          512 bytes per deque block; MSVC's STL packs `sizeof(T) <= 1 ? 16 : <= 2 ? 8 : <= 4 ? 4 :
 *          <= 8 ? 2 : 1` elements per block (`<deque>`, `_Deque_val::_Block_size`) -- a heap
 *          allocation and a free per element for any type wider than 8 bytes, one every two for a
 *          `std::coroutine_handle<>`. Measured (Huly QB-215): the ask-cost probe's `stream` mode at
 *          69 ns per chunk on Windows against 31 for a bare push (24 vs 24 on Linux), the coroutine
 *          sync primitives 1.3-2x slower than g++ with 64-512 parked waiters, `channel::try_send` /
 *          `try_recv` 1.7-1.9x. This ring is what they use instead: a power-of-two buffer over storage
 *          aligned for `T`, doubled when full (the elements are MOVED, never copied), elements
 *          destroyed on `pop_front`, `erase_at` and destruction. Deque-shaped names, plus `operator[]`
 *          and `erase_at` for the rare cancellation retract that scans for its own entry. Single
 *          thread by contract, like everything it is used in.
 * @ingroup Container
 */

#ifndef QB_SYSTEM_CONTAINER_GROWABLE_RING_H
#define QB_SYSTEM_CONTAINER_GROWABLE_RING_H

#include <cstddef>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace qb {

/**
 * @class growable_ring
 * @ingroup Container
 * @brief A growable single-thread FIFO ring of `T`: no allocation per element.
 * @tparam T The element type; moved on growth, so move-constructible. `erase_at` needs it
 *           move-assignable.
 */
template <typename T>
class growable_ring {
    T          *_buf  = nullptr;
    std::size_t _cap  = 0; ///< a power of two; 0 until the first element
    std::size_t _head = 0;
    std::size_t _size = 0;

    static T *
    allocate(std::size_t n) {
        return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }
    static void
    deallocate(T *p) noexcept {
        ::operator delete(p, std::align_val_t{alignof(T)}); // unsized: the -fno-sized-deallocation axis
    }
    [[nodiscard]] std::size_t
    slot(std::size_t i) const noexcept {
        return (_head + i) & (_cap - 1);
    }
    void
    grow() {
        const std::size_t ncap = _cap ? _cap * 2 : 8;
        T                *nb   = allocate(ncap);
        for (std::size_t i = 0; i < _size; ++i) {
            T *src = _buf + slot(i);
            std::construct_at(nb + i, std::move(*src));
            std::destroy_at(src);
        }
        deallocate(_buf);
        _buf  = nb;
        _cap  = ncap;
        _head = 0;
    }

public:
    using value_type = T;

    /// A forward iterator over the held elements, oldest first (index-based, so `erase_at` on the
    /// current index and a fresh `begin()` is the way to erase while walking).
    template <bool Const>
    class basic_iterator {
        using ring_t   = std::conditional_t<Const, const growable_ring, growable_ring>;
        ring_t     *_r = nullptr;
        std::size_t _i = 0;

    public:
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::conditional_t<Const, const T &, T &>;
        using pointer           = std::conditional_t<Const, const T *, T *>;
        using iterator_category = std::forward_iterator_tag;
        basic_iterator()        = default;
        basic_iterator(ring_t *r, std::size_t i) noexcept
            : _r(r)
            , _i(i) {}
        reference
        operator*() const noexcept {
            return (*_r)[_i];
        }
        pointer
        operator->() const noexcept {
            return &(*_r)[_i];
        }
        basic_iterator &
        operator++() noexcept {
            ++_i;
            return *this;
        }
        basic_iterator
        operator++(int) noexcept {
            auto c = *this;
            ++_i;
            return c;
        }
        bool
        operator==(const basic_iterator &o) const noexcept {
            return _i == o._i;
        }
        bool
        operator!=(const basic_iterator &o) const noexcept {
            return _i != o._i;
        }
        [[nodiscard]] std::size_t
        index() const noexcept {
            return _i;
        }
    };
    using iterator       = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;
    iterator
    begin() noexcept {
        return {this, 0};
    }
    iterator
    end() noexcept {
        return {this, _size};
    }
    const_iterator
    begin() const noexcept {
        return {this, 0};
    }
    const_iterator
    end() const noexcept {
        return {this, _size};
    }

    growable_ring()                                 = default;
    growable_ring(const growable_ring &)            = delete;
    growable_ring &operator=(const growable_ring &) = delete;
    growable_ring(growable_ring &&o) noexcept
        : _buf(std::exchange(o._buf, nullptr))
        , _cap(std::exchange(o._cap, 0))
        , _head(std::exchange(o._head, 0))
        , _size(std::exchange(o._size, 0)) {}
    growable_ring &
    operator=(growable_ring &&o) noexcept {
        if (this != &o) {
            clear();
            deallocate(_buf);
            _buf  = std::exchange(o._buf, nullptr);
            _cap  = std::exchange(o._cap, 0);
            _head = std::exchange(o._head, 0);
            _size = std::exchange(o._size, 0);
        }
        return *this;
    }
    ~growable_ring() {
        clear();
        deallocate(_buf);
    }

    [[nodiscard]] bool
    empty() const noexcept {
        return _size == 0;
    }
    [[nodiscard]] std::size_t
    size() const noexcept {
        return _size;
    }
    [[nodiscard]] std::size_t
    capacity() const noexcept {
        return _cap;
    }
    /// The oldest element; the ring must not be empty.
    [[nodiscard]] T &
    front() noexcept {
        return _buf[_head];
    }
    [[nodiscard]] const T &
    front() const noexcept {
        return _buf[_head];
    }
    /// The i-th element from the front (0 = `front()`); `i < size()`.
    [[nodiscard]] T &
    operator[](std::size_t i) noexcept {
        return _buf[slot(i)];
    }
    [[nodiscard]] const T &
    operator[](std::size_t i) const noexcept {
        return _buf[slot(i)];
    }
    template <typename... Args>
    T &
    emplace_back(Args &&...args) {
        if (_size == _cap)
            grow();
        T *p = std::construct_at(_buf + slot(_size), std::forward<Args>(args)...);
        ++_size;
        return *p;
    }
    void
    push_back(const T &v) {
        emplace_back(v);
    }
    void
    push_back(T &&v) {
        emplace_back(std::move(v));
    }
    /// Destroys the oldest element; the ring must not be empty.
    void
    pop_front() noexcept {
        std::destroy_at(_buf + _head);
        _head = (_head + 1) & (_cap - 1);
        --_size;
    }
    /// Removes the i-th element, keeping the order of the others (moves the tail down by one).
    void
    erase_at(std::size_t i) noexcept {
        for (std::size_t j = i; j + 1 < _size; ++j)
            _buf[slot(j)] = std::move(_buf[slot(j + 1)]);
        std::destroy_at(_buf + slot(_size - 1));
        --_size;
    }
    /// Deque-shaped erase: removes the element the iterator names, returns an iterator at the same index
    /// (the element that followed, or `end()`).
    iterator
    erase(iterator it) noexcept {
        erase_at(it.index());
        return {this, it.index()};
    }
    void
    clear() noexcept {
        while (_size)
            pop_front();
    }
};

} // namespace qb

#endif // QB_SYSTEM_CONTAINER_GROWABLE_RING_H
