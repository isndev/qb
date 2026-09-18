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
 *          destroyed on `pop_front`, `erase_at` and destruction. The cursors are pointers (`_head`,
 *          `_tail`, `_end`), so a push or a pop is a construct or a destroy, one compare against the
 *          end and one increment -- the same instruction budget as a deque's block cursor, which is
 *          what keeps the try-send / try-recv loop of a channel at its libstdc++ speed. Deque-shaped
 *          names, plus `operator[]` and `erase_at` for the rare cancellation retract that scans for
 *          its own entry. Single thread by contract, like everything it is used in.
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
    T          *_buf  = nullptr; ///< the storage, `_end - _buf` slots (a power of two; 0 until the first element)
    T          *_end  = nullptr; ///< one past the storage
    T          *_head = nullptr; ///< the oldest element
    T          *_tail = nullptr; ///< the next free slot
    std::size_t _size = 0;

    static T *
    allocate(std::size_t n) {
        return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }
    static void
    deallocate(T *p) noexcept {
        ::operator delete(p, std::align_val_t{alignof(T)}); // unsized: the -fno-sized-deallocation axis
    }
    [[nodiscard]] T *
    at(std::size_t i) const noexcept { // the i-th element from the head, wrapping once
        T *p = _head + i;
        if (p >= _end)
            p -= (_end - _buf);
        return p;
    }
    void
    grow() {
        const std::size_t cap  = static_cast<std::size_t>(_end - _buf);
        const std::size_t ncap = cap ? cap * 2 : 8;
        T                *nb   = allocate(ncap);
        for (std::size_t i = 0; i < _size; ++i) {
            T *src = at(i);
            std::construct_at(nb + i, std::move(*src));
            std::destroy_at(src);
        }
        deallocate(_buf);
        _buf  = nb;
        _end  = nb + ncap;
        _head = nb;
        _tail = nb + _size;
    }
    void
    release() noexcept {
        clear();
        deallocate(_buf);
        _buf = _end = _head = _tail = nullptr;
    }

public:
    using value_type = T;

    /// A forward iterator over the held elements, oldest first (index-based: `erase(it)` erases the
    /// element it names and returns an iterator at the same index).
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

    growable_ring()                                 = default;
    growable_ring(const growable_ring &)            = delete;
    growable_ring &operator=(const growable_ring &) = delete;
    growable_ring(growable_ring &&o) noexcept
        : _buf(std::exchange(o._buf, nullptr))
        , _end(std::exchange(o._end, nullptr))
        , _head(std::exchange(o._head, nullptr))
        , _tail(std::exchange(o._tail, nullptr))
        , _size(std::exchange(o._size, 0)) {}
    growable_ring &
    operator=(growable_ring &&o) noexcept {
        if (this != &o) {
            release();
            _buf  = std::exchange(o._buf, nullptr);
            _end  = std::exchange(o._end, nullptr);
            _head = std::exchange(o._head, nullptr);
            _tail = std::exchange(o._tail, nullptr);
            _size = std::exchange(o._size, 0);
        }
        return *this;
    }
    ~growable_ring() {
        release();
    }

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
        return static_cast<std::size_t>(_end - _buf);
    }
    /// The oldest element; the ring must not be empty.
    [[nodiscard]] T &
    front() noexcept {
        return *_head;
    }
    [[nodiscard]] const T &
    front() const noexcept {
        return *_head;
    }
    /// The i-th element from the front (0 = `front()`); `i < size()`.
    [[nodiscard]] T &
    operator[](std::size_t i) noexcept {
        return *at(i);
    }
    [[nodiscard]] const T &
    operator[](std::size_t i) const noexcept {
        return *at(i);
    }
    template <typename... Args>
    T &
    emplace_back(Args &&...args) {
        if (_size == static_cast<std::size_t>(_end - _buf))
            grow();
        T *p = std::construct_at(_tail, std::forward<Args>(args)...);
        if (++_tail == _end)
            _tail = _buf;
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
        std::destroy_at(_head);
        if (++_head == _end)
            _head = _buf;
        --_size;
    }
    /// Removes the i-th element, keeping the order of the others (moves the tail down by one).
    void
    erase_at(std::size_t i) noexcept {
        for (std::size_t j = i; j + 1 < _size; ++j)
            *at(j) = std::move(*at(j + 1));
        if (_tail == _buf)
            _tail = _end;
        --_tail;
        std::destroy_at(_tail);
        --_size;
    }
    /// Deque-shaped erase: removes the element the iterator names, returns an iterator at the same
    /// index (the element that followed, or `end()`).
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
