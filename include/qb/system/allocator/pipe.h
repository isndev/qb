/**
 * @file qb/system/allocator/pipe.h
 * @brief Implementation of a dynamic extensible buffer
 *
 * This file defines the `base_pipe` and `pipe` classes that implement an extensible
 * buffer capable of storing data efficiently with automatic capacity management. These
 * classes are widely used in the asynchronous I/O system for managing input and output
 * data.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - C++ Actor Framework (cpp.actor)
 */

#ifndef QB_PIPE_H
#define QB_PIPE_H
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <qb/string.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/prefix.h>
#include <string_view>
#include <vector>

namespace qb::allocator {

/**
 * @brief Calculates the number of elements of type U needed to store an element of type
 * T
 *
 * @tparam T Type of the element to store
 * @tparam U Base type of the pipe
 * @return Number of elements of type U needed
 */
template <typename T, typename U>
constexpr auto
getItemSize() {
    return sizeof(T) / sizeof(U) + static_cast<bool>(sizeof(T) % sizeof(U));
}

/**
 * @class base_pipe
 * @brief Base class for the extensible buffer
 *
 * Implements the fundamental functionalities of an extensible buffer:
 * - Automatic memory allocation and deallocation
 * - Capacity management with automatic resizing
 * - Efficient data access for reading and writing
 * - Methods for allocating space at the beginning or end of the buffer
 *
 * @tparam T Type of elements stored in the buffer
 */
template <typename T>
class base_pipe : std::allocator<T> {
    using base_type                          = std::allocator<T>;
    constexpr static const std::size_t _SIZE = 4096; /**< Initial buffer size */

protected:
    std::size_t _begin;    /**< Index of first valid element */
    std::size_t _end;      /**< Index just after the last valid element */
    bool _flag_front;      /**< Indicates if the last allocation was at the beginning */
    std::size_t _capacity; /**< Total buffer capacity */
    std::size_t _factor;   /**< Buffer expansion factor */
    T          *_data;     /**< Buffer data */

public:
    /**
     * @brief Default constructor
     *
     * Initializes an empty buffer with default capacity
     */
    base_pipe()
        : _begin(0)
        , _end(0)
        , _flag_front(false)
        , _capacity(_SIZE)
        , _factor(1)
        , _data(base_type::allocate(_SIZE)) {}

    /**
     * @brief Copy constructor
     *
     * Copies data from another buffer
     *
     * @param rhs Buffer to copy from
     */
    base_pipe(base_pipe const &rhs)
        : base_type()
        , _begin(0)
        , _end(0)
        , _flag_front(false)
        , _capacity(0)
        , _factor(1)
        , _data(nullptr) {
        std::memcpy(allocate_back(rhs.size()), rhs._data, rhs.size() * sizeof(T));
    }

    /**
     * @brief Move constructor
     *
     * Transfers ownership of data from another buffer
     *
     * @param rhs Buffer to move from
     */
    base_pipe(base_pipe &&rhs) noexcept
        : base_type()
        , _begin(rhs._begin)
        , _end(rhs._end)
        , _flag_front(rhs._flag_front)
        , _capacity(rhs._capacity)
        , _factor(rhs._factor)
        , _data(rhs._data) {
        rhs._begin = rhs._end = 0;
        rhs._flag_front       = false;
        rhs._capacity         = 0;
        rhs._factor           = 1;
        rhs._data             = nullptr;
    }

    /**
     * @brief Copy assignment operator
     */
    base_pipe &operator=(base_pipe const &rhs) {
        if (this != &rhs) {
            reset();
            std::memcpy(allocate_back(rhs.size()), rhs._data, rhs.size() * sizeof(T));
        }
        return *this;
    };

    /**
     * @brief Move assignment operator
     *
     * @param rhs Buffer to move from
     * @return Reference to this buffer after move
     */
    base_pipe &
    operator=(base_pipe &&rhs) noexcept {
        base_type::deallocate(_data, _capacity);
        _begin      = rhs._begin;
        _end        = rhs._end;
        _flag_front = rhs._flag_front;
        _capacity   = rhs._capacity;
        _factor     = rhs._factor;
        _data       = rhs._data;
        rhs._begin = rhs._end = 0;
        rhs._flag_front       = false;
        rhs._capacity         = 0;
        rhs._factor           = 1;
        rhs._data             = nullptr;
        return *this;
    }

    /**
     * @brief Destructor
     *
     * Frees memory allocated by the buffer
     */
    ~base_pipe() {
        if (_capacity)
            base_type::deallocate(_data, _capacity);
    }

    /**
     * @brief Returns the total capacity of the buffer
     *
     * @return Capacity in number of elements
     */
    [[nodiscard]] inline std::size_t
    capacity() const noexcept {
        return _capacity;
    }

    /**
     * @brief Returns a pointer to the buffer data
     *
     * @return Pointer to the data
     */
    [[nodiscard]] inline T *
    data() const noexcept {
        return _data;
    }

    /**
     * @brief Returns a pointer to the first valid element
     *
     * @return Pointer to the beginning of valid data
     */
    [[nodiscard]] inline T *
    begin() const noexcept {
        return _data + _begin;
    }

    /**
     * @brief Returns a pointer just after the last valid element
     *
     * @return Pointer to the end of valid data
     */
    [[nodiscard]] inline T *
    end() const noexcept {
        return _data + _end;
    }

    /**
     * @brief Returns a constant pointer to the first valid element
     *
     * @return Constant pointer to the beginning of valid data
     */
    [[nodiscard]] inline const T *
    cbegin() const noexcept {
        return _data + _begin;
    }

    /**
     * @brief Returns a constant pointer just after the last valid element
     *
     * @return Constant pointer to the end of valid data
     */
    [[nodiscard]] inline const T *
    cend() const noexcept {
        return _data + _end;
    }

    /**
     * @brief Returns the number of valid elements in the buffer
     *
     * @return Number of elements
     */
    [[nodiscard]] inline std::size_t
    size() const noexcept {
        return _end - _begin;
    }

    /**
     * @brief Resizes the buffer
     *
     * If the new size is smaller than the current size, excess elements are removed.
     * If the new size is larger, space is allocated at the end of the buffer.
     *
     * @param new_size Desired new size
     */
    void
    resize(std::size_t new_size) {
        if (new_size <= size())
            _end -= size() - new_size;
        else
            allocate_back(new_size - size());
    }

    /**
     * @brief Frees elements at the beginning of the buffer
     *
     * @param size Number of elements to free
     */
    inline void
    free_front(std::size_t const size) noexcept {
        _begin += size;
    }

    /**
     * @brief Frees elements at the end of the buffer
     *
     * @param size Number of elements to free
     */
    inline void
    free_back(std::size_t const size) noexcept {
        _end -= size;
    }

    /**
     * @brief Resets the buffer to a specific position
     *
     * If the specified position is not the end, sets the beginning to this position.
     * Otherwise, completely resets the buffer.
     *
     * @param begin New beginning position
     */
    inline void
    reset(std::size_t const begin) noexcept {
        if (begin != _end)
            _begin = begin;
        else {
            _begin = 0;
            _end   = 0;
        }
    }

    /**
     * @brief Completely resets the buffer
     *
     * Data remains in memory but is marked as invalid
     */
    inline void
    reset() noexcept {
        _begin      = 0;
        _end        = 0;
        _flag_front = false;
    }

    /**
     * @brief Clears the buffer content (alias for reset)
     */
    inline void
    clear() noexcept {
        reset();
    }

    /**
     * @brief Checks if the buffer is empty
     *
     * @return true if the buffer is empty, false otherwise
     */
    inline bool
    empty() const noexcept {
        return _begin == _end;
    }

    /**
     * @brief Frees a specified number of elements
     *
     * Depending on the _flag_front flag, frees at the beginning or end of the buffer
     *
     * @param size Number of elements to free
     */
    inline void
    free(std::size_t const size) noexcept {
        if (_flag_front)
            _begin += size;
        else
            _end -= size;
    }

    /**
     * @brief Allocates space at the end of the buffer
     *
     * Automatically increases capacity if necessary.
     *
     * @param size Number of elements to allocate
     * @return Pointer to the beginning of the allocated space
     */
    inline auto *
    allocate_back(std::size_t const size) {
        if (likely(_end + size <= _capacity)) {
            const auto save_index = _end;
            _end += size;
            return _data + save_index;
        }
        const auto nb_item = _end - _begin;
        const auto half    = _capacity / 2;
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
            if (_capacity)
                base_type::deallocate(_data, _capacity);

            _begin    = 0;
            _end      = nb_item + size;
            _capacity = new_capacity;
            _data     = new_data;
        }
        return _data + nb_item;
    }

    /**
     * @brief Allocates and constructs an object of type U at the end of the buffer
     *
     * @tparam U Type of object to construct
     * @tparam _Init Types of arguments for construction
     * @param init Arguments for construction
     * @return Reference to the constructed object
     */
    template <typename U, typename... _Init>
    inline U &
    allocate_back(_Init &&...init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate_back(BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    /**
     * @brief Allocates space with a custom size and constructs an object
     *
     * @tparam U Type of object to construct
     * @tparam _Init Types of arguments for construction
     * @param size Additional size to allocate after the object
     * @param init Arguments for construction
     * @return Reference to the constructed object
     */
    template <typename U, typename... _Init>
    inline U &
    allocate_size(std::size_t const size, _Init &&...init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate_back(size + BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    /**
     * @brief Allocates space at the beginning or end of the buffer
     *
     * Chooses the location based on available space
     *
     * @param size Number of elements to allocate
     * @return Pointer to the beginning of the allocated space
     */
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

    /**
     * @brief Allocates space and constructs an object
     *
     * @tparam U Type of object to construct
     * @tparam _Init Types of arguments for construction
     * @param init Arguments for construction
     * @return Reference to the constructed object
     */
    template <typename U, typename... _Init>
    inline U &
    allocate(_Init &&...init) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *(new (reinterpret_cast<U *>(allocate(BUCKET_SIZE)))
                     U(std::forward<_Init>(init)...));
    }

    /**
     * @brief Copies an object to the end of the buffer
     *
     * @tparam U Type of object to copy
     * @param data Object to copy
     * @return Reference to the copy of the object
     */
    template <typename U>
    inline U &
    recycle_back(U const &data) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *reinterpret_cast<U *>(
            std::memcpy(allocate_back(BUCKET_SIZE), &data, sizeof(U)));
    }

    /**
     * @brief Copies data to the end of the buffer
     *
     * @tparam U Type of data to copy
     * @param data Data to copy
     * @param size Number of elements to copy
     * @return Reference to the copy of the data
     */
    template <typename U>
    inline U &
    recycle_back(U const &data, std::size_t const size) {
        return *reinterpret_cast<U *>(
            std::memcpy(allocate_back(size), &data, size * sizeof(T)));
    }

    /**
     * @brief Copies an object to the beginning or end of the buffer
     *
     * @tparam U Type of object to copy
     * @param data Object to copy
     * @return Reference to the copy of the object
     */
    template <typename U>
    inline U &
    recycle(U const &data) {
        constexpr std::size_t BUCKET_SIZE = getItemSize<U, T>();
        return *reinterpret_cast<U *>(
            std::memcpy(allocate(BUCKET_SIZE), &data, sizeof(U)));
    }

    /**
     * @brief Copies data to the beginning or end of the buffer
     *
     * @tparam U Type of data to copy
     * @param data Data to copy
     * @param size Number of elements to copy
     * @return Reference to the copy of the data
     */
    template <typename U>
    inline U &
    recycle(U const &data, std::size_t const size) {
        return *reinterpret_cast<U *>(
            std::memcpy(allocate(size), &data, size * sizeof(T)));
    }

    /**
     * @brief Reorganizes the buffer to consolidate free space
     *
     * Moves data to the beginning of the buffer
     */
    inline void
    reorder() noexcept {
        if (!_begin)
            return;
        const auto nb_item = _end - _begin;
        // std::cout << "Start reorder " << _begin << ":" << _end << "|" << nb_item;
        std::memmove(_data, _data + _begin, nb_item * sizeof(T));
        _begin = 0;
        _end   = nb_item;
        // std::cout << "End reorder " << _begin << ":" << _end << "|" << _end - _begin;
    }

    /**
     * @brief Flushes the buffer (synchronization operation, no-op in this class)
     */
    inline void
    flush() const noexcept {}

    /**
     * @brief Reserves space in the buffer
     *
     * Allocates space without marking it as used
     *
     * @param size Number of elements to reserve
     */
    void
    reserve(std::size_t const size) {
        allocate_back(size);
        free_back(size);
    }
};

/**
 * @class pipe
 * @brief Extensible buffer optimized for performance
 *
 * Implements an extensible buffer aligned on cache lines
 * for optimal performance in multithreaded contexts.
 *
 * @tparam T Type of elements stored in the buffer
 */
template <typename T>
class QB_LOCKFREE_CACHELINE_ALIGNMENT pipe : public base_pipe<T> {
public:
    /**
     * @brief Swaps content with another buffer
     *
     * @param rhs Other buffer for the swap
     */
    inline void
    swap(pipe &rhs) noexcept {
        std::swap(*reinterpret_cast<CacheLine *>(this),
                  *reinterpret_cast<CacheLine *>(&rhs));
    }

    /**
     * @brief Adds elements from an iterator range
     *
     * @tparam _It Iterator type
     * @param begin Begin iterator
     * @param end End iterator
     * @return Reference to this buffer
     */
    template <typename _It>
    pipe &
    put(_It begin, _It const &end) {
        auto out = this->allocate_back(end - begin);
        while (begin != end) {
            *out = *begin;
            ++out;
            ++begin;
        }
        return *this;
    }

    /**
     * @brief Adds elements from an array
     *
     * @param data Pointer to the data
     * @param size Number of elements
     * @return Reference to this buffer
     */
    pipe &
    put(T const *data, std::size_t const size) {
        memcpy(this->allocate_back(size), data, size * sizeof(T));
        return *this;
    }

    /**
     * @brief Adds an element to the buffer (operator <<)
     *
     * @tparam U Type of element to add
     * @param rhs Element to add
     * @return Reference to this buffer
     */
    template <typename U>
    pipe &
    operator<<(U &&rhs) noexcept {
        this->template allocate_back<U>(std::forward<U>(rhs));
        return *this;
    }
};

/**
 * @class pipe<char>
 * @brief Specialization of the extensible buffer for characters
 *
 * Adds functionality specific to text manipulation.
 */
template <>
class pipe<char> : public base_pipe<char> {
public:
    pipe() = default;
    pipe(pipe const &) = default;
    pipe(pipe &&) noexcept = default;
    pipe &operator=(pipe const &) = default;
    pipe &operator=(pipe &&) noexcept = default;

    /**
     * @brief Adds a value converted to text
     *
     * @tparam U Type of the value
     * @param rhs Value to convert and add
     * @return Reference to this buffer
     */
    template <typename U>
    pipe &
    put(U &rhs) {
        return put(static_cast<const U &>(rhs));
    }

    /**
     * @brief Adds a constant value converted to text
     *
     * @tparam U Type of the value
     * @param rhs Value to convert and add
     * @return Reference to this buffer
     */
    template <typename U>
    pipe &
    put(const U &rhs) {
        return put(std::to_string(rhs));
    }

    /**
     * @brief Adds a literal string
     *
     * @tparam _Size Size of the string
     * @param str String to add
     * @return Reference to this buffer
     */
    template <std::size_t _Size>
    pipe &
    put(const char (&str)[_Size]) {
        memcpy(allocate_back(_Size - 1), str, _Size - 1);
        return *this;
    }

    /**
     * @brief Adds a QB string
     *
     * @tparam _Size Size of the string
     * @param str String to add
     * @return Reference to this buffer
     */
    template <std::size_t _Size>
    pipe &
    put(const qb::string<_Size> &str) {
        memcpy(allocate_back(str.size()), str.c_str(), str.size());
        return *this;
    }

    /**
     * @brief Adds a vector of elements
     *
     * @tparam T Type of elements in the vector
     * @param vec Vector to add
     * @return Reference to this buffer
     */
    template <typename T>
    pipe &
    put(std::vector<T> const &vec) {
        memcpy(allocate_back(vec.size()), reinterpret_cast<const char *>(vec.data()),
               vec.size());
        return *this;
    }

    /**
     * @brief Adds a static array of elements
     *
     * @tparam T Type of elements in the array
     * @tparam _Size Size of the array
     * @param arr Array to add
     * @return Reference to this buffer
     */
    template <typename T, std::size_t _Size>
    pipe &
    put(std::array<T, _Size> const &arr) {
        memcpy(allocate_back(arr.size()), reinterpret_cast<const char *>(arr.data()),
               arr.size());
        return *this;
    }

    /**
     * @brief Adds elements from an iterator range
     *
     * @tparam _It Iterator type
     * @param begin Begin iterator
     * @param end End iterator
     * @return Reference to this buffer
     */
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

    /**
     * @brief Adds a value to the buffer (operator <<)
     *
     * @tparam U Type of the value
     * @param rhs Value to add
     * @return Reference to this buffer
     */
    template <typename U>
    pipe &
    operator<<(const U &rhs) {
        return put(rhs);
    }

    /**
     * @brief Adds a reference to the buffer (operator <<)
     *
     * @tparam U Type of the value
     * @param rhs Reference to add
     * @return Reference to this buffer
     */
    template <typename U>
    pipe &
    operator<<(U &rhs) {
        return put(rhs);
    }

    /**
     * @brief Adds raw data to the buffer
     *
     * @param data Pointer to the data
     * @param size Size of the data
     * @return Reference to this buffer
     */
    pipe &put(char const *data, std::size_t size) noexcept;

    /**
     * @brief Writes raw data to the buffer
     *
     * @param data Pointer to the data
     * @param size Size of the data
     * @return Reference to this buffer
     */
    pipe &write(const char *data, std::size_t size) noexcept;

    /**
     * @brief Converts the content to std::string
     *
     * @return Copy of the content as std::string
     */
    std::string str() const noexcept;

    /**
     * @brief Creates a view of the content
     *
     * @return View of the content (without copying)
     */
    std::string_view view() const noexcept;
};

// Explicit specialization declarations
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

/**
 * @brief Stream operator to display the content of a pipe<char>
 *
 * @tparam stream Type of output stream
 * @param os Output stream
 * @param p Pipe to display
 * @return Reference to the output stream
 */
template <typename stream, typename = std::enable_if_t<
                               !std::is_same_v<stream, qb::allocator::pipe<char>>>>
stream &
operator<<(stream &os, qb::allocator::pipe<char> const &p) {
    os << std::string_view(p.begin(), p.size());
    return os;
}

#endif // QB_PIPE_H