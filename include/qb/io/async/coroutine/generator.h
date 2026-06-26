/**
 * @file qb/io/async/coroutine/generator.h
 * @brief Generator coroutines with co_yield
 *
 * Generators produce sequences of values lazily.
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
 * @ingroup Coroutine
 */

#ifndef QB_IO_ASYNC_COROUTINE_GENERATOR_H
#define QB_IO_ASYNC_COROUTINE_GENERATOR_H

#include <cassert>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <optional>

/** Set QB_DEBUG_AGEN=1 (compile flag or before the include) to enable
 *  async_generator trace prints that show the yield/next/suspend flow. */
#if defined(QB_DEBUG_AGEN) && QB_DEBUG_AGEN
// Standard C++20 __VA_OPT__ elides the comma when no trailing args are passed
// (MSVC needs the conformant preprocessor /Zc:preprocessor, enabled by qb's build).
#define QB_AGEN_TRACE(fmt, ...) std::fprintf(stderr, "[agen ] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define QB_AGEN_TRACE(fmt, ...) (void) 0
#endif

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
        std::optional<T>   current_value;
        std::exception_ptr exception;

        auto
        get_return_object() {
            return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always
        initial_suspend() {
            return {};
        }
        std::suspend_always
        final_suspend() noexcept {
            return {};
        }

        void
        unhandled_exception() {
            exception = std::current_exception();
        }

        void
        return_void() {}

        std::suspend_always
        yield_value(T value) {
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
    explicit generator(handle_type h)
        : _handle(h) {}

    ~generator() {
        if (_handle) {
            _handle.destroy();
        }
    }

    // Move-only
    generator(generator &&other) noexcept
        : _handle(std::exchange(other._handle, {})) {}
    generator &
    operator=(generator &&other) noexcept {
        if (this != &other) {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    generator(const generator &)            = delete;
    generator &operator=(const generator &) = delete;

    /**
     * @brief Iterator for range-based for loops
     */
    class iterator {
        handle_type _handle;

    public:
        iterator() noexcept
            : _handle(nullptr) {}
        explicit iterator(handle_type h)
            : _handle(h) {
            if (_handle && !_handle.done()) {
                _handle.resume();
            }
            rethrow_if_failed();
            // Empty generator: coroutine runs to co_return, no yields → treat as end
            if (!_handle || _handle.done()) {
                _handle = nullptr;
            }
        }

        bool
        operator!=(const iterator &other) const noexcept {
            return _handle != other._handle;
        }

        bool
        operator==(const iterator &other) const noexcept {
            return _handle == other._handle;
        }

        iterator &
        operator++() {
            if (_handle && !_handle.done()) {
                _handle.resume();
            }
            rethrow_if_failed();
            if (!_handle || _handle.done()) {
                _handle = nullptr;
            }
            return *this;
        }

        const T &
        operator*() const {
            // Finding 2.A.4: operator* must not dereference a null optional.
            // Callers are supposed to check `it != end()` first, but misuse
            // should fail loudly instead of silently returning garbage.
            assert(_handle && _handle.promise().current_value.has_value() && "generator::iterator operator* on exhausted iterator");
            return *_handle.promise().current_value;
        }

        const T *
        operator->() const {
            assert(_handle && _handle.promise().current_value.has_value() && "generator::iterator operator-> on exhausted iterator");
            return &*_handle.promise().current_value;
        }

    private:
        // Surface a generator-body exception to the consuming loop instead of
        // silently ending iteration (the previous behavior made a throwing
        // generator indistinguishable from a normally exhausted one).
        void
        rethrow_if_failed() {
            if (_handle && _handle.promise().exception) {
                auto ex = _handle.promise().exception;
                _handle = nullptr; // iteration ends; do not resume a failed frame
                std::rethrow_exception(ex);
            }
        }
    };

    iterator
    begin() {
        // Null check first: a moved-from generator has a null handle and
        // calling done() on it is undefined behavior.
        if (!_handle || _handle.done()) {
            return end();
        }
        return iterator{_handle};
    }

    iterator
    end() {
        return iterator{};
    }

    /**
     * @brief Check if generator has more values
     */
    bool
    has_next() const {
        return _handle && !_handle.done();
    }

    /**
     * @brief Get next value (advances generator)
     * @return Optional value - empty if done
     */
    std::optional<T>
    next() {
        if (!_handle) {
            return std::nullopt;
        }

        // Exception check must come BEFORE the done() check: a generator that
        // threw is also done(), and the previous order returned nullopt forever
        // without ever surfacing the stored exception.
        if (_handle.promise().exception) {
            // Clear as we rethrow (mirrors async_generator::next): a throw ends the stream, so a
            // consumer that catches and calls next() again must get nullopt, not the same throw.
            std::rethrow_exception(std::exchange(_handle.promise().exception, nullptr));
        }
        if (_handle.done()) {
            return std::nullopt;
        }

        // At initial_suspend we have no value yet; resume to first yield
        if (!_handle.promise().current_value.has_value()) {
            _handle.resume();
            if (_handle.promise().exception)
                std::rethrow_exception(std::exchange(_handle.promise().exception, nullptr));
            if (_handle.done())
                return std::nullopt;
        }

        auto result = _handle.promise().current_value;
        _handle.resume(); // Advance past this yield so has_next() is false after
                          // the last value. If this resume throws into the
                          // promise, the exception surfaces on the NEXT call
                          // (top-of-function check) — the value produced here
                          // is still legitimately delivered.
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
        std::optional<T>        current_value;
        std::exception_ptr      exception;
        std::coroutine_handle<> continuation;

        auto
        get_return_object() {
            return async_generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always
        initial_suspend() {
            return {};
        }

        auto
        final_suspend() noexcept {
            QB_AGEN_TRACE("final_suspend continuation=%p", continuation ? (void *) continuation.address() : nullptr);
            struct final_awaiter {
                std::coroutine_handle<> continuation;
                bool
                await_ready() const noexcept {
                    return false;
                }
                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<>) noexcept {
                    return continuation ? continuation : std::noop_coroutine();
                }
                void
                await_resume() noexcept {}
            };
            return final_awaiter{continuation};
        }

        void
        unhandled_exception() {
            exception = std::current_exception();
        }

        void
        return_void() {}

        /**
         * @brief Symmetric-transfer yield: resumes the awaiting consumer
         * (ag_for_each / ag_collect etc.) inline instead of going through
         * the scheduler.  Previously returned std::suspend_always which
         * left both the generator AND the consumer permanently suspended.
         */
        auto
        yield_value(T value) {
            current_value = std::move(value);
            QB_AGEN_TRACE("yield_value value ready, transferring to continuation=%p", continuation ? (void *) continuation.address() : nullptr);
            struct yield_awaiter {
                std::coroutine_handle<> cont;
                bool
                await_ready() const noexcept {
                    return false;
                }
                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<>) noexcept {
                    return cont ? cont : std::noop_coroutine();
                }
                void
                await_resume() noexcept {}
            };
            return yield_awaiter{continuation};
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

private:
    handle_type _handle;

public:
    explicit async_generator(handle_type h)
        : _handle(h) {}

    ~async_generator() {
        if (_handle)
            _handle.destroy();
    }

    async_generator(async_generator &&other) noexcept
        : _handle(std::exchange(other._handle, {})) {}

    async_generator &
    operator=(async_generator &&other) noexcept {
        if (this != &other) {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    async_generator(const async_generator &)            = delete;
    async_generator &operator=(const async_generator &) = delete;

    struct next_awaiter {
        handle_type handle;

        bool
        await_ready() const {
            return false;
        }

        /**
         * @brief Symmetric transfer to the generator.
         *
         * Returning the generator's handle (instead of calling handle.resume()
         * directly) is critical for two reasons:
         *
         * 1. Stack safety: avoids N levels of nested resume() calls for N
         *    yielded values.
         *
         * 2. Use-after-free prevention: with a direct handle.resume() call,
         *    the completion chain (generator → ag_for_each → caller task) can
         *    trigger ~task<void>() which calls handle.destroy() BEFORE
         *    await_suspend() returns, leaving a dangling handle that the
         *    post-resume code would then dereference (SIGSEGV / ASAN UAF).
         *
         * With symmetric transfer the coroutine machinery ensures that
         * await_suspend() returns IMMEDIATELY (tail-call) without any further
         * access to `handle` in this frame.
         */
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> h) noexcept {
            QB_AGEN_TRACE("next await_suspend consumer=%p gen=%p", (void *) h.address(), (void *) handle.address());
            handle.promise().continuation = h;
            return handle; // symmetric transfer — do NOT access handle after this
        }

        std::optional<T>
        await_resume() {
            QB_AGEN_TRACE("next await_resume gen=%p done=%d", (void *) handle.address(), handle.done() ? 1 : 0);
            // Surface a generator-body exception BEFORE the done() short-circuit. A throw inside the
            // generator runs unhandled_exception() and then drives the frame to final_suspend, so the
            // frame is `done()` at the same time the exception is stored. Checking done() first would
            // silently swallow that exception (next() would return nullopt as if the stream ended
            // cleanly) — and every ag_* helper consumes via `while (co_await gen.next())`, so the throw
            // would never reach the caller. Rethrow first; only a clean end-of-stream returns nullopt.
            if (handle.promise().exception) {
                // Clear the stored exception as we rethrow it: a throw ends the stream, so a
                // consumer that catches and (incorrectly) calls next() again must get a clean
                // nullopt rather than the same exception re-thrown forever.
                std::rethrow_exception(std::exchange(handle.promise().exception, nullptr));
            }
            if (handle.done()) {
                return std::nullopt;
            }
            return std::move(handle.promise().current_value);
        }
    };

    next_awaiter
    next() {
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
std::vector<T>
collect_to_vector(generator<T> &gen) {
    std::vector<T> result;
    for (auto &val : gen) {
        result.push_back(std::move(val));
    }
    return result;
}

/**
 * @brief Create generator from range
 * @tparam Range Range type
 * @param range Source range (taken by value — see note)
 * @return Generator yielding range elements
 *
 * Finding 2.A.8 — lifetime contract:
 *   The parameter is taken **by value** so that the range is copied/moved
 *   into the coroutine frame. Taking a forwarding reference here would
 *   dangle: coroutine frames only extend the lifetime of function
 *   parameters *passed by value*, so a reference to a temporary like
 *   `from_range(std::vector{1,2,3})` would be pointing at freed memory
 *   by the time the first `co_yield` suspends.
 *
 *   If you want to iterate without copying, pass a `std::reference_wrapper`
 *   or a view type (`std::span`, `std::ranges::subrange`) and ensure the
 *   underlying container outlives the generator.
 *
 * @ingroup Coroutine
 */
template <typename Range>
auto
from_range(Range range) -> generator<typename std::remove_cvref_t<Range>::value_type> {
    for (auto &item : range) {
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
generator<T>
empty_generator() {
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
generator<T>
single_generator(T value) {
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
auto
from_iterator(Iter begin, Iter end) -> generator<typename std::iterator_traits<Iter>::value_type> {
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
generator<T>
iota(T start) {
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
generator<T>
range(T start, T end) {
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
generator<T>
repeat(T value) {
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
generator<T>
repeat_n(T value, size_t count) {
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
generator<T>
concat(generator<T> first, generator<T> second) {
    for (auto &val : first) {
        co_yield std::move(val);
    }
    for (auto &val : second) {
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
generator<T>
take(generator<T> gen, size_t count) {
    size_t i = 0;
    for (auto &val : gen) {
        if (i++ >= count)
            break;
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
generator<T>
skip(generator<T> gen, size_t count) {
    size_t i = 0;
    for (auto &val : gen) {
        if (i++ < count)
            continue;
        co_yield std::move(val);
    }
}

// =============================================================================
// async_generator<T> helper algorithms
// =============================================================================

/**
 * @brief Consume every value from an async_generator
 *
 * Calls f(*value) for each value produced. f may be a regular function or
 * a coroutine (returning task<void>).
 *
 * @code
 * async_generator<int> produce() { ... }
 *
 * co_await ag_for_each(produce(), [](int v) -> task<void> {
 *     co_await process(v);
 * });
 * @endcode
 * @ingroup Coroutine
 */
template <typename T, typename F>
task<void>
ag_for_each(async_generator<T> gen, F f) {
    QB_AGEN_TRACE("ag_for_each start");
    [[maybe_unused]] std::size_t n = 0;
    while (auto val = co_await gen.next()) {
        QB_AGEN_TRACE("ag_for_each got item #%zu", n++);
        if constexpr (std::is_same_v<std::invoke_result_t<F, T &>, task<void>>) {
            co_await f(*val);
        } else {
            f(*val);
        }
    }
    QB_AGEN_TRACE("ag_for_each done after %zu items", n);
}

/**
 * @brief Collect all values from an async_generator into a vector
 * @code
 * std::vector<int> v = co_await ag_collect(produce());
 * @endcode
 * @ingroup Coroutine
 */
template <typename T>
task<std::vector<T>>
ag_collect(async_generator<T> gen) {
    QB_AGEN_TRACE("ag_collect start");
    std::vector<T> result;
    while (auto val = co_await gen.next()) {
        QB_AGEN_TRACE("ag_collect got item total=%zu", result.size() + 1);
        result.push_back(std::move(*val));
    }
    QB_AGEN_TRACE("ag_collect done total=%zu", result.size());
    co_return result;
}

/**
 * @brief Map each value with f, collecting into a vector
 * @code
 * auto doubled = co_await ag_map(produce(), [](int v){ return v * 2; });
 * @endcode
 * @ingroup Coroutine
 */
template <typename T, typename F>
task<std::vector<std::invoke_result_t<F, T>>>
ag_map(async_generator<T> gen, F f) {
    using R = std::invoke_result_t<F, T>;
    std::vector<R> result;
    while (auto val = co_await gen.next())
        result.push_back(f(std::move(*val)));
    co_return result;
}

/**
 * @brief Filter values, collecting matching ones into a vector
 * @code
 * auto evens = co_await ag_filter(produce(), [](int v){ return v % 2 == 0; });
 * @endcode
 * @ingroup Coroutine
 */
template <typename T, typename Pred>
task<std::vector<T>>
ag_filter(async_generator<T> gen, Pred pred) {
    std::vector<T> result;
    while (auto val = co_await gen.next())
        if (pred(*val))
            result.push_back(std::move(*val));
    co_return result;
}

/**
 * @brief Fold / reduce all values into a single accumulator
 *
 * @param gen     Source generator
 * @param init    Initial accumulator value
 * @param reducer Binary function: (Acc, T) -> Acc
 * @code
 * int sum = co_await ag_reduce(produce(), 0, std::plus<int>{});
 * @endcode
 * @ingroup Coroutine
 */
template <typename T, typename Acc, typename F>
task<Acc>
ag_reduce(async_generator<T> gen, Acc init, F reducer) {
    while (auto val = co_await gen.next())
        init = reducer(std::move(init), std::move(*val));
    co_return init;
}

/**
 * @brief Take at most N values from an async_generator
 * @ingroup Coroutine
 */
template <typename T>
async_generator<T>
ag_take(async_generator<T> gen, size_t n) {
    size_t count = 0;
    while (count < n) {
        auto val = co_await gen.next();
        if (!val)
            break;
        co_yield std::move(*val);
        ++count;
    }
}

/**
 * @brief Skip the first N values from an async_generator
 * @ingroup Coroutine
 */
template <typename T>
async_generator<T>
ag_skip(async_generator<T> gen, size_t n) {
    size_t skipped = 0;
    while (auto val = co_await gen.next()) {
        if (skipped++ < n)
            continue;
        co_yield std::move(*val);
    }
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_GENERATOR_H
