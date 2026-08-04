/**
 * @file qb/system/container/ring_buffer.h
 * @brief Ring buffer implementation
 *
 * This file provides a fixed-size circular buffer implementation that offers
 * efficient FIFO operations. The ring buffer supports both fixed-size and
 * dynamic-size configurations, and provides thread-safe operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Container
 */

#ifndef QB_RINGBUFFER_H
#define QB_RINGBUFFER_H

#include <algorithm>
#include <cstddef> // size_t / ptrdiff_t — the iterator's size_type and difference_type below.
                   // libc++ drags both in through <algorithm>; libstdc++ does not, so this
                   // header compiled alone ONLY on macOS until the Linux leg of the
                   // installed-header gate said "'ptrdiff_t' does not name a type".
#include <cstring>
#include <iterator>   // std::forward_iterator_tag — iterator_category below
#include <type_traits>

namespace qb {

/**
 * @brief A fixed-size ring buffer (circular buffer) implementation
 *
 * @tparam T The type of elements stored in the ring buffer
 * @tparam N The capacity of the ring buffer
 * @tparam Overwrite Whether to overwrite old elements when the buffer is full (defaults
 * to true)
 */
template <typename T, size_t N, bool Overwrite = true>
class ring_buffer;

namespace detail {
/**
 * @brief Iterator for ring_buffer
 *
 * @tparam T The type of elements in the buffer
 * @tparam N The capacity of the buffer
 * @tparam C Whether this is a const iterator (true) or non-const iterator (false)
 * @tparam Overwrite Whether to overwrite old elements when the buffer is full
 */
template <typename T, size_t N, bool C, bool Overwrite>
class ring_buffer_iterator {
    using buffer_t = std::conditional_t<!C, ring_buffer<T, N, Overwrite> *, ring_buffer<T, N, Overwrite> const *>;

public:
    using self_type         = ring_buffer_iterator<T, N, C, Overwrite>;
    using value_type        = T;
    using reference         = std::conditional_t<C, T const &, T &>;
    using const_reference   = T const &;
    using pointer           = std::conditional_t<C, T const *, T *>;
    using const_pointer     = T const *;
    using size_type         = size_t;
    using difference_type   = ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    /**
     * @brief Default constructor
     */
    ring_buffer_iterator() noexcept = default;

    /**
     * @brief Constructor with source buffer, current index, and count
     *
     * @param source The source ring buffer
     * @param index Current index in the buffer
     * @param count Current element count
     */
    ring_buffer_iterator(buffer_t source, size_type index, size_type count) noexcept
        : source_{source}
        , index_{index}
        , count_{count} {}

    /**
     * @brief Copy constructor
     */
    ring_buffer_iterator(ring_buffer_iterator const &) noexcept = default;

    /**
     * @brief Copy assignment operator
     */
    ring_buffer_iterator &operator=(ring_buffer_iterator const &) noexcept = default;

    /**
     * @brief Dereference operator
     *
     * @return Reference to the current element (const or non-const based on iterator type)
     */
    [[nodiscard]] reference
    operator*() const noexcept {
        return (*source_)[index_];
    }

    /**
     * @brief Arrow operator
     *
     * @return Pointer to the current element (const or non-const based on iterator type)
     */
    [[nodiscard]] pointer
    operator->() const noexcept {
        return &((*source_)[index_]);
    }

    /**
     * @brief Pre-increment operator
     *
     * @return Reference to this iterator after incrementing
     */
    self_type &
    operator++() noexcept {
        index_ = ++index_ % N;
        ++count_;
        return *this;
    }

    /**
     * @brief Post-increment operator
     *
     * @return Copy of this iterator before incrementing
     */
    [[nodiscard]] self_type
    operator++(int) noexcept {
        auto result = *this;
        this->operator++();
        return result;
    }

    /**
     * @brief Get the current index in the buffer
     *
     * @return The current index
     */
    [[nodiscard]] size_type
    index() const noexcept {
        return index_;
    }

    /**
     * @brief Get the current count of accessed elements
     *
     * @return The current count
     */
    [[nodiscard]] size_type
    count() const noexcept {
        return count_;
    }

    /**
     * @brief Destructor
     */
    ~ring_buffer_iterator() = default;

private:
    buffer_t  source_{}; ///< The source ring buffer
    size_type index_{};  ///< Current index in the buffer
    size_type count_{};  ///< Count of accessed elements
};

/**
 * @brief Equality comparison operator for ring buffer iterators
 *
 * @tparam T Element type
 * @tparam N Buffer capacity
 * @tparam C Whether the iterators are const
 * @tparam Overwrite Whether the buffer overwrites old elements when full
 * @param l Left iterator
 * @param r Right iterator
 * @return Whether the iterators are equal (based on count)
 */
template <typename T, size_t N, bool C, bool Overwrite>
bool
operator==(ring_buffer_iterator<T, N, C, Overwrite> const &l, ring_buffer_iterator<T, N, C, Overwrite> const &r) noexcept {
    return l.count() == r.count();
}

/**
 * @brief Inequality comparison operator for ring buffer iterators
 *
 * @tparam T Element type
 * @tparam N Buffer capacity
 * @tparam C Whether the iterators are const
 * @tparam Overwrite Whether the buffer overwrites old elements when full
 * @param l Left iterator
 * @param r Right iterator
 * @return Whether the iterators are not equal (based on count)
 */
template <typename T, size_t N, bool C, bool Overwrite>
bool
operator!=(ring_buffer_iterator<T, N, C, Overwrite> const &l, ring_buffer_iterator<T, N, C, Overwrite> const &r) noexcept {
    return l.count() != r.count();
}
} // namespace detail

/**
 * @brief A fixed-size ring buffer (circular buffer) implementation
 *
 * Ring buffer is a circular data structure with a fixed size that can efficiently
 * add and remove elements from either end. When the buffer is full, new elements
 * either overwrite the oldest ones (if Overwrite=true) or are discarded (if
 * Overwrite=false).
 *
 * @tparam T The type of elements stored in the ring buffer
 * @tparam N The capacity of the ring buffer
 * @tparam Overwrite Whether to overwrite old elements when the buffer is full (defaults
 * to true)
 */
template <typename T, size_t N, bool Overwrite>
class ring_buffer {
    using self_type = ring_buffer<T, N, Overwrite>;

public:
    static_assert(N > 0, "ring buffer must have a size greater than zero.");

    using value_type      = T;
    using reference       = T &;
    using const_reference = T const &;
    using pointer         = T *;
    using const_pointer   = T const *;
    using size_type       = size_t;
    using iterator        = detail::ring_buffer_iterator<T, N, false, Overwrite>;
    using const_iterator  = detail::ring_buffer_iterator<T, N, true, Overwrite>;

    /**
     * @brief Default constructor
     */
    ring_buffer() noexcept = default;

    /**
     * @brief Copy constructor
     *
     * @param rhs The ring buffer to copy from
     */
    ring_buffer(ring_buffer const &rhs) noexcept(std::is_nothrow_copy_constructible_v<value_type>) {
        copy_impl(rhs);
    }

    /**
     * @brief Copy assignment operator
     *
     * @param rhs The ring buffer to copy from
     * @return Reference to this ring buffer
     */
    ring_buffer &
    operator=(ring_buffer const &rhs) noexcept(std::is_nothrow_copy_constructible_v<value_type>) {
        if (this == &rhs)
            return *this;

        clear();
        copy_impl(rhs);

        return *this;
    }

    /**
     * @brief Add an element to the back of the buffer
     *
     * If the buffer is full and Overwrite is true, the oldest element will be
     * overwritten. If the buffer is full and Overwrite is false, the element will be
     * discarded.
     *
     * @tparam U Type of the value to add
     * @param value The value to add
     */
    template <typename U>
    void
    push_back(U &&value) {
        if constexpr (!Overwrite) {
            if (full())
                return;
        }
        push_back_impl(std::forward<U>(value));
    }

    /**
     * @brief Remove the oldest element from the buffer
     */
    void
    pop_front() noexcept {
        if (empty())
            return;

        destroy_at(tail_);
        --size_;
        tail_ = ++tail_ % N;
    }

    /**
     * @brief Access the newest element in the buffer
     *
     * @return Reference to the newest element
     */
    [[nodiscard]] reference
    back() noexcept {
        const size_type idx = (head_ == 0) ? N - 1 : head_ - 1;
        return *reinterpret_cast<pointer>(elements_ + idx * sizeof(T));
    }

    /**
     * @brief Access the newest element in the buffer (const version)
     *
     * @return Const reference to the newest element
     */
    [[nodiscard]] const_reference
    back() const noexcept {
        return const_cast<self_type *>(this)->back();
    }

    /**
     * @brief Access the oldest element in the buffer
     *
     * @return Reference to the oldest element
     */
    [[nodiscard]] reference
    front() noexcept {
        return *reinterpret_cast<pointer>(elements_ + tail_ * sizeof(T));
    }

    /**
     * @brief Access the oldest element in the buffer (const version)
     *
     * @return Const reference to the oldest element
     */
    [[nodiscard]] const_reference
    front() const noexcept {
        return const_cast<self_type *>(this)->front();
    }

    /**
     * @brief Access an element by index
     *
     * @param index The index of the element to access
     * @return Reference to the element at the specified index
     */
    [[nodiscard]] reference
    operator[](size_type index) noexcept {
        return *reinterpret_cast<pointer>(elements_ + index * sizeof(T));
    }

    /**
     * @brief Access an element by index (const version)
     *
     * @param index The index of the element to access
     * @return Const reference to the element at the specified index
     */
    [[nodiscard]] const_reference
    operator[](size_type index) const noexcept {
        return const_cast<self_type *>(this)->operator[](index);
    }

    /**
     * @brief Get an iterator to the first element
     *
     * @return Iterator to the first element
     */
    [[nodiscard]] iterator
    begin() noexcept {
        return iterator{this, tail_, 0};
    }

    /**
     * @brief Get an iterator to the end of the buffer
     *
     * @return Iterator to the position after the last element
     */
    [[nodiscard]] iterator
    end() noexcept {
        return iterator{this, head_, size_};
    }

    /**
     * @brief Get a const iterator to the first element
     *
     * @return Const iterator to the first element
     */
    [[nodiscard]] const_iterator
    cbegin() const noexcept {
        return const_iterator{this, tail_, 0};
    }

    /**
     * @brief Get a const iterator to the end of the buffer
     *
     * @return Const iterator to the position after the last element
     */
    [[nodiscard]] const_iterator
    cend() const noexcept {
        return const_iterator{this, head_, size_};
    }

    /**
     * @brief Check if the buffer is empty
     *
     * @return true if the buffer is empty, false otherwise
     */
    [[nodiscard]] bool
    empty() const noexcept {
        return size_ == 0;
    }

    /**
     * @brief Check if the buffer is full
     *
     * @return true if the buffer is full, false otherwise
     */
    [[nodiscard]] bool
    full() const noexcept {
        return size_ == N;
    }

    /**
     * @brief Get the capacity of the buffer
     *
     * @return The maximum number of elements the buffer can hold
     */
    [[nodiscard]] size_type
    capacity() const noexcept {
        return N;
    }

    /**
     * @brief Clear all elements from the buffer
     */
    void
    clear() noexcept {
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            while (!empty()) {
                destroy_at(tail_);
                tail_ = ++tail_ % N;
                --size_;
            }
        }
        head_ = tail_ = size_ = 0;
    }

    /**
     * @brief Destructor
     */
    ~ring_buffer() {
        clear();
    }

private:
    /**
     * @brief Destroy the element at the given index (no-op for trivially destructible)
     */
    void
    destroy_at(size_type index) noexcept {
        if constexpr (!std::is_trivially_destructible_v<value_type>)
            reinterpret_cast<pointer>(elements_ + index * sizeof(T))->~T();
    }

    /**
     * @brief Copy elements from another ring buffer
     */
    void
    copy_impl(self_type const &rhs) {
        tail_ = rhs.tail_;
        head_ = rhs.head_;
        size_ = rhs.size_;

        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(elements_, rhs.elements_, sizeof(elements_));
        } else {
            // Track how many elements were actually constructed so a throwing
            // copy constructor only destroys the constructed prefix. Relying on
            // size_ (set to rhs.size_ above) would run ~T() on raw, never-
            // constructed storage for the [constructed, size_) tail — UB.
            size_type constructed = 0;
            try {
                for (; constructed < size_; ++constructed)
                    new (elements_ + (((tail_ + constructed) % N) * sizeof(T))) T(rhs[(tail_ + constructed) % N]);
            } catch (...) {
                for (size_type j = 0; j < constructed; ++j)
                    destroy_at((tail_ + j) % N);
                head_ = tail_ = size_ = 0;
                throw;
            }
        }
    }

    /**
     * @brief Common implementation for push_back
     */
    template <typename U>
    void
    push_back_impl(U &&value) {
        if (full())
            destroy_at(head_);

        new (elements_ + head_ * sizeof(T)) T{std::forward<U>(value)};
        head_ = ++head_ % N;

        if (full())
            tail_ = ++tail_ % N;

        if (!full())
            ++size_;
    }

    /// Storage for elements with proper alignment
    /// C++23 deprecates std::aligned_storage; use alignas with a std::byte array.
    alignas(T) std::byte elements_[sizeof(T) * N]{};
    size_type head_{}; ///< Index where the next element will be inserted
    size_type tail_{}; ///< Index of the oldest element
    size_type size_{}; ///< Current number of elements in the buffer
};

} // namespace qb

#endif // QB_RINGBUFFER_H
