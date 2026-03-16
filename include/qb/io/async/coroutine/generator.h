/**
 * @file qb/io/async/coroutine/generator.h
 * @brief Generator coroutines with co_yield
 *
 * Generators produce sequences of values lazily.
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
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_GENERATOR_H
#define QB_IO_ASYNC_COROUTINE_GENERATOR_H

#include <coroutine>
#include <optional>
#include <exception>

namespace qb::io::async {

/**
 * @brief Generator coroutine type
 * @tparam T Value type produced by generator
 *
 * Generators produce values on-demand using co_yield.
 * Unlike tasks, they can produce multiple values.
 *
 * Usage:
 * @code
 * generator<int> fibonacci(int n) {
 *     int a = 0, b = 1;
 *     for (int i = 0; i < n; ++i) {
 *         co_yield a;
 *         auto next = a + b;
 *         a = b;
 *         b = next;
 *     }
 * }
 *
 * // Consumer
 * for (auto val : fibonacci(10)) {
 *     std::cout << val << " ";
 * }
 * @endcode
 *
 * Lifetime: The generator object must outlive any iteration (begin/end and
 * iterator use). Destroying the generator while iterating leaves the iterator
 * with a dangling handle.
 */
template <typename T>
class generator {
public:
    struct promise_type {
        std::optional<T> current_value;
        std::exception_ptr exception;

        auto get_return_object() {
            return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void unhandled_exception() {
            exception = std::current_exception();
        }

        void return_void() {}

        std::suspend_always yield_value(T value) {
            current_value = std::move(value);
            return {};
        }

        // Disallow co_await inside generators
        void await_transform() = delete;
    };

    using handle_type = std::coroutine_handle<promise_type>;

private:
    handle_type _handle;

public:
    explicit generator(handle_type h) : _handle(h) {}

    ~generator() {
        if (_handle) {
            _handle.destroy();
        }
    }

    // Move-only
    generator(generator&& other) noexcept : _handle(std::exchange(other._handle, {})) {}
    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            if (_handle) _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    /**
     * @brief Iterator for range-based for loops
     */
    class iterator {
        handle_type _handle;

    public:
        iterator() noexcept : _handle(nullptr) {}
        explicit iterator(handle_type h) : _handle(h) {
            if (_handle && !_handle.done()) {
                _handle.resume();
            }
            // Empty generator: coroutine runs to co_return, no yields → treat as end
            if (!_handle || _handle.done()) {
                _handle = nullptr;
            }
        }

        bool operator!=(const iterator& other) const noexcept {
            return _handle != other._handle;
        }

        bool operator==(const iterator& other) const noexcept {
            return _handle == other._handle;
        }

        iterator& operator++() {
            if (_handle && !_handle.done()) {
                _handle.resume();
            }
            if (!_handle || _handle.done()) {
                _handle = nullptr;
            }
            return *this;
        }

        const T& operator*() const {
            return *_handle.promise().current_value;
        }

        const T* operator->() const {
            return &*_handle.promise().current_value;
        }
    };

    iterator begin() {
        if (_handle.done()) {
            return end();
        }
        return iterator{_handle};
    }

    iterator end() {
        return iterator{};
    }

    /**
     * @brief Check if generator has more values
     */
    bool has_next() const {
        return _handle && !_handle.done();
    }

    /**
     * @brief Get next value (advances generator)
     * @return Optional value - empty if done
     */
    std::optional<T> next() {
        if (!_handle || _handle.done()) {
            return std::nullopt;
        }

        if (_handle.promise().exception) {
            std::rethrow_exception(_handle.promise().exception);
        }

        // At initial_suspend we have no value yet; resume to first yield
        if (!_handle.promise().current_value.has_value()) {
            _handle.resume();
            if (_handle.done()) return std::nullopt;
        }

        auto result = _handle.promise().current_value;
        _handle.resume();  // Advance past this yield so has_next() is false after last value
        return result;
    }
};

/**
 * @brief Async generator that can co_await
 * @tparam T Value type
 *
 * Like generator but can suspend on async operations.
 */
template <typename T>
class async_generator {
public:
    struct promise_type {
        std::optional<T> current_value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        auto get_return_object() {
            return async_generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() { return {}; }

        auto final_suspend() noexcept {
            struct final_awaiter {
                std::coroutine_handle<> continuation;

                bool await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
                    return continuation ? continuation : std::noop_coroutine();
                }
                void await_resume() noexcept {}
            };
            return final_awaiter{continuation};
        }

        void unhandled_exception() {
            exception = std::current_exception();
        }

        void return_void() {}

        std::suspend_always yield_value(T value) {
            current_value = std::move(value);
            return {};
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

private:
    handle_type _handle;

public:
    explicit async_generator(handle_type h) : _handle(h) {}

    ~async_generator() {
        if (_handle) _handle.destroy();
    }

    async_generator(async_generator&& other) noexcept
        : _handle(std::exchange(other._handle, {})) {}

    async_generator& operator=(async_generator&& other) noexcept {
        if (this != &other) {
            if (_handle) _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    async_generator(const async_generator&) = delete;
    async_generator& operator=(const async_generator&) = delete;

    struct next_awaiter {
        handle_type handle;
        std::optional<T> result;

        bool await_ready() const { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            handle.promise().continuation = h;
            handle.resume();
        }

        std::optional<T> await_resume() {
            if (handle.done()) {
                return std::nullopt;
            }
            if (handle.promise().exception) {
                std::rethrow_exception(handle.promise().exception);
            }
            return std::move(handle.promise().current_value);
        }
    };

    next_awaiter next() {
        return next_awaiter{_handle};
    }
};

/**
 * @brief Helper to collect generator values into vector
 * @tparam T Value type
 * @param gen Generator to drain
 * @return Vector of all values
 * @ingroup Coroutine
 */
template <typename T>
std::vector<T> collect_to_vector(generator<T>& gen) {
    std::vector<T> result;
    for (auto& val : gen) {
        result.push_back(std::move(val));
    }
    return result;
}

/**
 * @brief Create generator from range
 * @tparam Range Range type
 * @param range Source range
 * @return Generator yielding range elements
 * @ingroup Coroutine
 */
template <typename Range>
auto from_range(Range&& range)
    -> generator<typename std::remove_cvref_t<Range>::value_type> {
    for (auto& item : range) {
        co_yield item;
    }
}

/**
 * @brief Generator that yields nothing (empty sequence)
 * @tparam T Value type
 * @return Empty generator
 * @ingroup Coroutine
 */
template <typename T>
generator<T> empty_generator() {
    co_return;
}

/**
 * @brief Generator that yields single value
 * @tparam T Value type
 * @param value Value to yield
 * @return Generator with single element
 * @ingroup Coroutine
 */
template <typename T>
generator<T> single_generator(T value) {
    co_yield std::move(value);
}

/**
 * @brief Generator that yields values from iterator range
 * @tparam Iter Iterator type
 * @param begin Start iterator
 * @param end End iterator
 * @return Generator yielding range
 * @ingroup Coroutine
 */
template <typename Iter>
auto from_iterator(Iter begin, Iter end) -> generator<typename std::iterator_traits<Iter>::value_type> {
    for (auto it = begin; it != end; ++it) {
        co_yield *it;
    }
}

/**
 * @brief Infinite generator starting from value
 * @tparam T Value type
 * @param start Starting value
 * @return Generator yielding start, start+1, start+2, ...
 * @ingroup Coroutine
 */
template <typename T>
generator<T> iota(T start) {
    T current = start;
    while (true) {
        co_yield current++;
    }
}

/**
 * @brief Finite generator from start to end (exclusive)
 * @tparam T Value type
 * @param start Starting value
 * @param end Ending value (exclusive)
 * @return Generator yielding [start, end)
 * @ingroup Coroutine
 */
template <typename T>
generator<T> range(T start, T end) {
    for (T i = start; i < end; ++i) {
        co_yield i;
    }
}

/**
 * @brief Repeat a value infinitely
 * @tparam T Value type
 * @param value Value to repeat
 * @return Infinite generator
 * @ingroup Coroutine
 */
template <typename T>
generator<T> repeat(T value) {
    while (true) {
        co_yield value;
    }
}

/**
 * @brief Repeat a value N times
 * @tparam T Value type
 * @param value Value to repeat
 * @param count Number of repetitions
 * @return Generator with N elements
 * @ingroup Coroutine
 */
template <typename T>
generator<T> repeat_n(T value, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        co_yield value;
    }
}

/**
 * @brief Concatenate two generators
 * @tparam T Value type
 * @param first First generator
 * @param second Second generator
 * @return Generator yielding first then second
 * @ingroup Coroutine
 */
template <typename T>
generator<T> concat(generator<T> first, generator<T> second) {
    for (auto& val : first) {
        co_yield std::move(val);
    }
    for (auto& val : second) {
        co_yield std::move(val);
    }
}

/**
 * @brief Take N elements from generator
 * @tparam T Value type
 * @param gen Source generator
 * @param count Maximum elements to take
 * @return Generator with at most count elements
 * @ingroup Coroutine
 */
template <typename T>
generator<T> take(generator<T> gen, size_t count) {
    size_t i = 0;
    for (auto& val : gen) {
        if (i++ >= count) break;
        co_yield std::move(val);
    }
}

/**
 * @brief Skip N elements from generator
 * @tparam T Value type
 * @param gen Source generator
 * @param count Elements to skip
 * @return Generator skipping first count elements
 * @ingroup Coroutine
 */
template <typename T>
generator<T> skip(generator<T> gen, size_t count) {
    size_t i = 0;
    for (auto& val : gen) {
        if (i++ < count) continue;
        co_yield std::move(val);
    }
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_GENERATOR_H
