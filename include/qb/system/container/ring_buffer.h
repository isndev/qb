/**
 * @file qb/system/container/ring_buffer.h
 * @brief Ring buffer implementation
 *
 * This file provides a fixed-size circular buffer implementation that offers
 * efficient FIFO operations. The ring buffer supports both fixed-size and
 * dynamic-size configurations, and provides thread-safe operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
#include <cstring>
#include <iostream>
#include <type_traits>
#include <vector>

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
    using buffer_t =
        typename std::conditional<!C, ring_buffer<T, N, Overwrite> *,
                                  ring_buffer<T, N, Overwrite> const *>::type;

public:
    using self_type         = ring_buffer_iterator<T, N, C, Overwrite>;
    using value_type        = T;
    using reference         = T &;
    using const_reference   = T const &;
    using pointer           = T *;
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
     * @brief Dereference operator (non-const version)
     *
     * @return Reference to the current element
     */
    template <bool Z = C, typename std::enable_if<(!Z), int>::type * = nullptr>
    [[nodiscard]] reference
    operator*() noexcept {
        return (*source_)[index_];
    }

    /**
     * @brief Dereference operator (const version)
     *
     * @return Const reference to the current element
     */
    template <bool Z = C, typename std::enable_if<(Z), int>::type * = nullptr>
    [[nodiscard]] const_reference
    operator*() const noexcept {
        return (*source_)[index_];
    }

    /**
     * @brief Arrow operator (non-const version)
     *
     * @return Pointer to the current element
     */
    template <bool Z = C, typename std::enable_if<(!Z), int>::type * = nullptr>
    [[nodiscard]] reference
    operator->() noexcept {
        return &((*source_)[index_]);
    }

    /**
     * @brief Arrow operator (const version)
     *
     * @return Const pointer to the current element
     */
    template <bool Z = C, typename std::enable_if<(Z), int>::type * = nullptr>
    [[nodiscard]] const_reference
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
operator==(ring_buffer_iterator<T, N, C, Overwrite> const &l,
           ring_buffer_iterator<T, N, C, Overwrite> const &r) noexcept {
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
operator!=(ring_buffer_iterator<T, N, C, Overwrite> const &l,
           ring_buffer_iterator<T, N, C, Overwrite> const &r) noexcept {
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
    ring_buffer(ring_buffer const &rhs) noexcept(
        std::is_nothrow_copy_constructible_v<value_type>) {
        copy_impl(rhs, std::bool_constant<std::is_trivially_copyable_v<T>>{});
    }

    /**
     * @brief Copy assignment operator
     *
     * @param rhs The ring buffer to copy from
     * @return Reference to this ring buffer
     */
    ring_buffer &
    operator=(ring_buffer const &rhs) noexcept(
        std::is_nothrow_copy_constructible_v<value_type>) {
        if (this == &rhs)
            return *this;

        destroy_all(std::bool_constant<std::is_trivially_copyable_v<T>>{});
        copy_impl(rhs, std::bool_constant<std::is_trivially_copyable_v<T>>{});

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
        push_back(std::forward<U>(value), std::bool_constant<Overwrite>{});
    }

    /**
     * @brief Remove the oldest element from the buffer
     */
    void
    pop_front() noexcept {
        if (empty())
            return;

        destroy(tail_,
                std::bool_constant<std::is_trivially_destructible_v<value_type>>{});

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
        return reinterpret_cast<reference>(
            elements_[std::clamp<size_type>(head_ - 1, 0UL, N - 1)]);
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
        return reinterpret_cast<reference>(elements_[tail_]);
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
        return reinterpret_cast<reference>(elements_[index]);
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
        destroy_all(std::bool_constant<std::is_trivially_destructible_v<value_type>>{});
    }

    /**
     * @brief Destructor
     */
    ~ring_buffer() {
        clear();
    };

private:
    /**
     * @brief Destroy all elements (trivially destructible version)
     *
     * No-op for trivially destructible types.
     *
     * @param tag Tag dispatch for trivially destructible types
     */
    void
    destroy_all(std::true_type) {}

    /**
     * @brief Destroy all elements (non-trivially destructible version)
     *
     * Calls the destructor for each element in the buffer.
     *
     * @param tag Tag dispatch for non-trivially destructible types
     */
    void
    destroy_all(std::false_type) {
        while (!empty()) {
            destroy(tail_,
                    std::bool_constant<std::is_trivially_destructible_v<value_type>>{});
            tail_ = ++tail_ % N;
            --size_;
        }
    }

    /**
     * @brief Copy implementation for trivially copyable types
     *
     * Uses memcpy for efficient copying of trivially copyable types.
     *
     * @param rhs The ring buffer to copy from
     * @param tag Tag dispatch for trivially copyable types
     */
    void
    copy_impl(self_type const &rhs, std::true_type) {
        std::memcpy(elements_, rhs.elements_, rhs.size_ * sizeof(T));
        size_ = rhs.size_;
        tail_ = rhs.tail_;
        head_ = rhs.head_;
    }

    /**
     * @brief Copy implementation for non-trivially copyable types
     *
     * Individually constructs each element by calling its copy constructor.
     *
     * @param rhs The ring buffer to copy from
     * @param tag Tag dispatch for non-trivially copyable types
     */
    void
    copy_impl(self_type const &rhs, std::false_type) {
        tail_ = rhs.tail_;
        head_ = rhs.head_;
        size_ = rhs.size_;

        try {
            for (auto i = 0; i < size_; ++i)
                new (elements_ + ((tail_ + i) % N)) T(rhs[tail_ + ((tail_ + i) % N)]);
        } catch (...) {
            while (!empty()) {
                destroy(
                    tail_,
                    std::bool_constant<std::is_trivially_destructible_v<value_type>>{});
                tail_ = ++tail_ % N;
                --size_;
            }
            throw;
        }
    }

    /**
     * @brief Push back implementation for Overwrite=true
     *
     * @tparam U Type of the value to add
     * @param value The value to add
     * @param tag Tag dispatch for Overwrite=true
     */
    template <typename U>
    void
    push_back(U &&value, std::true_type) {
        push_back_impl(std::forward<U>(value));
    }

    /**
     * @brief Push back implementation for Overwrite=false
     *
     * @tparam U Type of the value to add
     * @param value The value to add
     * @param tag Tag dispatch for Overwrite=false
     */
    template <typename U>
    void
    push_back(U &&value, std::false_type) {
        if (full() && !Overwrite)
            return;
        push_back_impl(std::forward<U>(value));
    }

    /**
     * @brief Common implementation for push_back
     *
     * @tparam U Type of the value to add
     * @param value The value to add
     */
    template <typename U>
    void
    push_back_impl(U &&value) {
        if (full())
            destroy(head_,
                    std::bool_constant<std::is_trivially_destructible_v<value_type>>{});

        new (elements_ + head_) T{std::forward<U>(value)};
        head_ = ++head_ % N;

        if (full())
            tail_ = ++tail_ % N;

        if (!full())
            ++size_;
    }

    /**
     * @brief Destroy an element (trivially destructible version)
     *
     * No-op for trivially destructible types.
     *
     * @param index Index of the element to destroy
     * @param tag Tag dispatch for trivially destructible types
     */
    void
    destroy(size_type index, std::true_type) noexcept {}

    /**
     * @brief Destroy an element (non-trivially destructible version)
     *
     * Calls the destructor for the element at the specified index.
     *
     * @param index Index of the element to destroy
     * @param tag Tag dispatch for non-trivially destructible types
     */
    void
    destroy(size_type index, std::false_type) noexcept {
        reinterpret_cast<pointer>(&elements_[index])->~T();
    }

    /// Storage for elements with proper alignment
    typename std::aligned_storage<sizeof(T), alignof(T)>::type elements_[N]{};
    size_type head_{}; ///< Index where the next element will be inserted
    size_type tail_{}; ///< Index of the oldest element
    size_type size_{}; ///< Current number of elements in the buffer
};

} // namespace qb

#endif // QB_RINGBUFFER_H
