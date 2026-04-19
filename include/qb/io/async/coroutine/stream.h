/**
 * @file qb/io/async/coroutine/stream.h
 * @brief Async stream processing
 *
 * Stream transforms and processing for coroutines.
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

#ifndef QB_IO_ASYNC_COROUTINE_STREAM_H
#define QB_IO_ASYNC_COROUTINE_STREAM_H

#include "task.h"
#include "channel.h"
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace qb::io::async {

/**
 * @brief Async stream of values
 * @tparam T Value type
 *
 * Streams provide functional-style transformations on async sequences.
 *
 * Usage:
 * @code
 * auto stream = async_stream<packet>::from_channel(ch)
 *     .filter([](auto& p) { return p.valid(); })
 *     .map([](auto& p) { return decrypt(p); })
 *     .take(100);
 *
 * co_await stream.for_each([](auto& p) {
 *     process(p);
 * });
 * @endcode
 */
template <typename T>
class async_stream {
public:
    using value_type = T;

private:
    // Source function - produces next value
    std::function<task<std::optional<T>>()> _next;

public:
    explicit async_stream(std::function<task<std::optional<T>>()> next)
        : _next(std::move(next)) {}

public:
    /**
     * @brief Create stream from channel (borrowed reference)
     *
     * @pre The channel @p ch must outlive the returned stream AND every
     *      coroutine that consumes it. Breaking this precondition is
     *      undefined behaviour (use-after-free).
     *
     * Finding 2.C.8 — lifetime contract:
     *   This overload exists for the performance-critical path where the
     *   caller already guarantees the channel outlives the stream (e.g.
     *   a channel member of the same owning object, or a stack-allocated
     *   channel in a single coroutine's scope). For anything that crosses
     *   coroutine boundaries or stores the stream in a member, **use
     *   `from_channel_shared()`** instead — it is only a single `shared_ptr`
     *   copy per stream and removes the UAF footgun entirely.
     *
     * @note We considered making this overload private. We kept it public
     *       because several callers (Redis streaming, pgsql cursors) rely
     *       on the zero-shared_ptr-overhead variant; the precondition is
     *       explicitly documented instead.
     */
    static async_stream from_channel(channel<T>& ch) {
        channel<T>* ch_ptr = &ch;
        return async_stream([ch_ptr]() -> task<std::optional<T>> {
            co_return co_await ch_ptr->recv();
        });
    }

    /**
     * @brief Create stream from a shared channel (lifetime-safe)
     *
     * The shared_ptr keeps the channel alive as long as any stream or
     * coroutine consuming it exists.
     */
    static async_stream from_channel_shared(std::shared_ptr<channel<T>> ch) {
        return async_stream([ch]() -> task<std::optional<T>> {
            co_return co_await ch->recv();
        });
    }

    /**
     * @brief Create stream from vector
     */
    static async_stream from_vector(const std::vector<T>& vec) {
        auto shared_vec = std::make_shared<std::vector<T>>(vec);
        auto index = std::make_shared<size_t>(0);

        return async_stream([shared_vec, index]() -> task<std::optional<T>> {
            if (*index >= shared_vec->size()) {
                co_return std::nullopt;
            }
            co_return (*shared_vec)[(*index)++];
        });
    }

    /**
     * @brief Empty stream
     */
    static async_stream empty() {
        return async_stream([]() -> task<std::optional<T>> {
            co_return std::nullopt;
        });
    }

    /**
     * @brief Single value stream
     */
    static async_stream single(T value) {
        auto shared = std::make_shared<std::optional<T>>(std::move(value));
        return async_stream([shared]() -> task<std::optional<T>> {
            auto result = std::move(*shared);
            *shared = std::nullopt;
            co_return result;
        });
    }

    /**
     * @brief Map transformation
     */
    template <typename F>
    auto map(F f) -> async_stream<std::invoke_result_t<F, T>> {
        using U = std::invoke_result_t<F, T>;

        auto source = _next;
        return async_stream<U>([source, f]() -> task<std::optional<U>> {
            auto opt = co_await source();
            if (!opt) co_return std::nullopt;
            co_return f(std::move(*opt));
        });
    }

    /**
     * @brief Filter transformation
     */
    template <typename P>
    async_stream filter(P predicate) {
        auto source = _next;
        return async_stream([source, predicate]() -> task<std::optional<T>> {
            while (true) {
                auto opt = co_await source();
                if (!opt) co_return std::nullopt;
                if (predicate(*opt)) co_return opt;
            }
        });
    }

    /**
     * @brief Take N elements
     */
    async_stream take(size_t n) {
        auto source = _next;
        auto remaining = std::make_shared<size_t>(n);

        return async_stream([source, remaining]() -> task<std::optional<T>> {
            if (*remaining == 0) co_return std::nullopt;
            --(*remaining);
            co_return co_await source();
        });
    }

    /**
     * @brief Skip N elements
     */
    async_stream skip(size_t n) {
        auto source = _next;
        auto to_skip = std::make_shared<size_t>(n);

        return async_stream([source, to_skip]() -> task<std::optional<T>> {
            while (*to_skip > 0) {
                auto opt = co_await source();
                if (!opt) co_return std::nullopt;
                --(*to_skip);
            }
            co_return co_await source();
        });
    }

    /**
     * @brief Chain with another stream
     */
    async_stream chain(async_stream other) {
        auto first = _next;
        auto second = other._next;
        auto using_second = std::make_shared<bool>(false);

        return async_stream([first, second, using_second]() -> task<std::optional<T>> {
            if (!*using_second) {
                auto opt = co_await first();
                if (opt) co_return opt;
                *using_second = true;
            }
            co_return co_await second();
        });
    }

    /**
     * @brief Buffer elements into batches
     */
    async_stream<std::vector<T>> buffer(size_t batch_size) {
        auto source = _next;
        auto buffer = std::make_shared<std::vector<T>>();
        buffer->reserve(batch_size);

        return async_stream<std::vector<T>>([source, buffer, batch_size]() -> task<std::optional<std::vector<T>>> {
            while (buffer->size() < batch_size) {
                auto opt = co_await source();
                if (!opt) break;
                buffer->push_back(std::move(*opt));
            }
            if (buffer->empty()) co_return std::nullopt;

            auto result = std::make_optional(std::vector<T>());
            result->swap(*buffer);
            co_return result;
        });
    }

    /**
     * @brief Debounce - emit only after quiet period
     * Emits the last value only after no new values arrive for the delay period
     */
    async_stream debounce(std::chrono::milliseconds delay) {
        auto source  = _next;
        // Finding 2.C.6: a truly unbounded channel lets a fast producer race
        // arbitrarily far ahead of a slow debounced consumer, defeating the
        // point of "debounce" and exposing an OOM surface. A small bounded
        // channel + cooperative backpressure from the producer is the right
        // default — the consumer's debounce loop drains what accumulates
        // during each quiet period anyway.
        constexpr size_t kDebounceChannelCapacity = 64;
        auto ch      = std::make_shared<channel<T>>(kDebounceChannelCapacity);
        auto started = std::make_shared<bool>(false);
        // Finding 2.C.4: surface source-stream exceptions to the consumer
        // instead of silently turning them into end-of-stream.
        auto source_error = std::make_shared<std::exception_ptr>();

        return async_stream(
            [source, ch, started, delay, source_error]() -> task<std::optional<T>> {
            // Lazy start: spawn producer on first pull
            if (!*started) {
                *started = true;
                coro_scheduler().spawn(
                    [](std::function<task<std::optional<T>>()> src,
                       std::shared_ptr<channel<T>> c,
                       std::shared_ptr<std::exception_ptr> err) -> task<void> {
                        try {
                            while (true) {
                                auto opt = co_await src();
                                if (!opt) break;
                                co_await c->send(std::move(*opt));
                            }
                        } catch (const channel_closed&) {
                            // Consumer closed the channel; this is the
                            // terminal state, not an error — stay silent.
                        } catch (...) {
                            *err = std::current_exception();
                        }
                        c->close();
                    }(source, ch, source_error));
            }

            // Block until at least one value arrives
            auto first = co_await ch->recv();
            if (!first) {
                // Producer ended: propagate a pending error (if any) before
                // reporting end-of-stream to the consumer.
                if (*source_error) std::rethrow_exception(*source_error);
                co_return std::nullopt;
            }

            T latest = std::move(*first);

            // Debounce loop: sleep, then drain anything buffered during the quiet period
            while (true) {
                co_await sleep(delay);

                bool got_new = false;
                while (auto val = ch->try_recv()) {
                    latest  = std::move(*val);
                    got_new = true;
                }

                if (got_new) continue;  // new values arrived — restart quiet period
                co_return std::move(latest);
            }
        });
    }

    /**
     * @brief Throttle - limit emission rate
     * Emits at most one value per interval
     */
    async_stream throttle(std::chrono::milliseconds interval) {
        auto source = _next;
        auto last_emit = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() - interval);

        return async_stream([source, last_emit, interval]() -> task<std::optional<T>> {
            while (true) {
                auto opt = co_await source();
                if (!opt) co_return std::nullopt;

                auto now = std::chrono::steady_clock::now();
                auto next_allowed = *last_emit + interval;

                if (now < next_allowed) {
                    co_await sleep(std::chrono::duration_cast<std::chrono::milliseconds>(next_allowed - now));
                }

                *last_emit = std::chrono::steady_clock::now();
                co_return opt;
            }
        });
    }

    // -----------------------------------------------------------------------
    // Terminal consumers
    //
    // All terminal methods are non-coroutine shims that move *this into a
    // private static coroutine helper.  This guarantees that the stream's
    // _next functor (and any callable passed alongside it) lives in the
    // coroutine frame rather than being accessed via a dangling `this`
    // pointer — the same pattern used by scheduler::spawn(Callable) to
    // handle temporary lambdas safely.
    // -----------------------------------------------------------------------

    /**
     * @brief Process each element with a sync or async callback.
     *
     * Supports both:
     *   - sync:  `[](T v) { ... }`
     *   - async: `[](T v) -> task<void> { co_await ...; }`
     */
    template <typename F>
    task<void> for_each(F f) {
        return for_each_impl(std::move(*this), std::move(f));
    }

    /**
     * @brief Collect all elements into a vector.
     */
    task<std::vector<T>> collect() {
        return collect_impl(std::move(*this));
    }

    /**
     * @brief Get the first element (or nullopt if the stream is empty).
     */
    task<std::optional<T>> first() {
        return first_impl(std::move(*this));
    }

    /**
     * @brief Reduce all elements to a single value.
     */
    template <typename F>
    task<T> reduce(F f, T initial) {
        return reduce_impl(std::move(*this), std::move(f), std::move(initial));
    }

    /**
     * @brief Count the number of elements.
     */
    task<size_t> count() {
        return count_impl(std::move(*this));
    }

    /**
     * @brief Return true if any element satisfies the predicate.
     */
    template <typename P>
    task<bool> any(P predicate) {
        return any_impl(std::move(*this), std::move(predicate));
    }

    /**
     * @brief Return true if all elements satisfy the predicate.
     */
    template <typename P>
    task<bool> all(P predicate) {
        return all_impl(std::move(*this), std::move(predicate));
    }

    /**
     * @brief Find the first element satisfying the predicate.
     */
    template <typename P>
    task<std::optional<T>> find(P predicate) {
        return find_impl(std::move(*this), std::move(predicate));
    }

    /**
     * @brief Drain all elements into a channel.
     *
     * @pre @p ch must outlive the coroutine produced by this call.
     */
    task<void> drain_to(channel<T>& ch) {
        return drain_to_impl(std::move(*this), &ch);
    }

    /**
     * @brief Backpressure-aware buffer - pauses source when buffer is full
     * @param max_buffer Maximum number of elements to buffer
     * @param acquire_semaphore Optional semaphore to acquire before each element (for backpressure)
     * @return Stream with backpressure support
     *
     * Implementation note: the background filler uses a static free function
     * (backpressure_fill_task) instead of a local lambda so the coroutine frame
     * stores fn/buffer/sem as VALUE parameters rather than a pointer to a local
     * lambda that would dangle after backpressure() returns.
     */
    async_stream backpressure(size_t max_buffer, std::shared_ptr<semaphore> acquire_semaphore = nullptr) {
        auto source = _next;
        auto buffer = std::make_shared<channel<T>>(max_buffer);
        auto sem    = acquire_semaphore ? acquire_semaphore : std::make_shared<semaphore>(max_buffer);

        coro_scheduler().spawn(backpressure_fill_task(std::move(source), buffer, sem));

        // Consumer: read from buffer and release one semaphore slot so the
        // producer may push the next item.
        return async_stream([buffer, sem]() -> task<std::optional<T>> {
            auto opt = co_await buffer->recv();
            if (opt) {
                sem->release();
            }
            co_return opt;
        });
    }

    // -----------------------------------------------------------------------
    // Private static coroutine helpers for terminal consumers.
    //
    // Each helper takes `async_stream<T>` and the callable **by value** so
    // both live inside the coroutine frame.  The public shim above simply
    // calls the helper with std::move(*this) — making the shim itself a
    // plain (non-coroutine) function with no `this`-dangling risk.
    // -----------------------------------------------------------------------

    template <typename F>
    static task<void> for_each_impl(async_stream<T> stream, F f) {
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) co_return;
            if constexpr (std::is_same_v<std::invoke_result_t<F, T>, task<void>>) {
                co_await f(std::move(*opt));
            } else {
                f(std::move(*opt));
            }
        }
    }

    static task<std::vector<T>> collect_impl(async_stream<T> stream) {
        std::vector<T> result;
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) break;
            result.push_back(std::move(*opt));
        }
        co_return result;
    }

    static task<std::optional<T>> first_impl(async_stream<T> stream) {
        co_return co_await stream._next();
    }

    template <typename F>
    static task<T> reduce_impl(async_stream<T> stream, F f, T initial) {
        T result = std::move(initial);
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) break;
            result = f(std::move(result), std::move(*opt));
        }
        co_return result;
    }

    static task<size_t> count_impl(async_stream<T> stream) {
        size_t n = 0;
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) break;
            ++n;
        }
        co_return n;
    }

    template <typename P>
    static task<bool> any_impl(async_stream<T> stream, P predicate) {
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) co_return false;
            if (predicate(*opt)) co_return true;
        }
    }

    template <typename P>
    static task<bool> all_impl(async_stream<T> stream, P predicate) {
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) co_return true;
            if (!predicate(*opt)) co_return false;
        }
    }

    template <typename P>
    static task<std::optional<T>> find_impl(async_stream<T> stream, P predicate) {
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) co_return std::nullopt;
            if (predicate(*opt)) co_return opt;
        }
    }

    static task<void> drain_to_impl(async_stream<T> stream, channel<T>* ch_ptr) {
        while (true) {
            auto opt = co_await stream._next();
            if (!opt) co_return;
            co_await ch_ptr->send(std::move(*opt));
        }
    }

    // Static free function: fn, buffer, sem are VALUE parameters in the
    // coroutine frame — no dangling-lambda risk even after backpressure() returns.
    //
    // Finding 2.C.5: the EOF branch now releases the already-acquired
    // permit before closing the buffer. Without this, the semaphore ends
    // each stream short by one permit; repeatedly re-using the same
    // semaphore (when the caller passes their own) would slowly starve
    // subsequent producers.
    static task<void> backpressure_fill_task(
        std::function<task<std::optional<T>>()> fn,
        std::shared_ptr<channel<T>> buffer,
        std::shared_ptr<semaphore> sem)
    {
        while (true) {
            co_await sem->acquire();
            auto opt = co_await fn();
            if (!opt) {
                sem->release();
                buffer->close();
                co_return;
            }
            co_await buffer->send(std::move(*opt));
        }
    }

    // Expose next function for composition helpers (zip, merge_streams)
    std::function<task<std::optional<T>>()>& next_fn() { return _next; }
};

/**
 * @brief Merge multiple streams into one using round-robin interleaving
 * @tparam T Value type
 * @param streams Streams to merge
 * @return Merged stream with interleaved elements
 * @ingroup Coroutine
 */
template <typename T>
async_stream<T> merge_streams(std::vector<async_stream<T>> streams) {
    if (streams.empty()) {
        return async_stream<T>::empty();
    }

    auto next_funcs = std::make_shared<std::vector<std::function<task<std::optional<T>>()>>>();
    for (auto& s : streams) {
        next_funcs->push_back(std::move(s.next_fn()));
    }
    auto index = std::make_shared<size_t>(0);
    auto active_count = std::make_shared<size_t>(streams.size());
    auto finished = std::make_shared<std::vector<bool>>(streams.size(), false);

    return async_stream<T>([next_funcs, index, active_count, finished]() -> task<std::optional<T>> {
        if (*active_count == 0) co_return std::nullopt;

        const size_t n = next_funcs->size();
        size_t start_idx = *index;

        do {
            size_t current_idx = *index;
            *index = (current_idx + 1) % n;

            if (!(*finished)[current_idx]) {
                auto opt = co_await (*next_funcs)[current_idx]();
                if (opt) {
                    co_return opt;
                } else {
                    (*finished)[current_idx] = true;
                    --(*active_count);
                    if (*active_count == 0) co_return std::nullopt;
                }
            }
        } while (*index != start_idx);

        // All streams exhausted
        co_return std::nullopt;
    });
}

/**
 * @brief Zip two streams together
 * @tparam T First type
 * @tparam U Second type
 * @param a First stream
 * @param b Second stream
 * @return Stream of pairs
 * @ingroup Coroutine
 */
template <typename T, typename U>
async_stream<std::pair<T, U>> zip(async_stream<T> a, async_stream<U> b) {
    auto next_a = std::move(a.next_fn());
    auto next_b = std::move(b.next_fn());

    return async_stream<std::pair<T, U>>([next_a, next_b]() -> task<std::optional<std::pair<T, U>>> {
        // Finding 2.C.16: short-circuit on the first stream's end-of-stream.
        // The previous implementation awaited `b` even after `a` was known
        // to be exhausted, which could block forever on a slow second
        // stream and waste one extra value from `b` (advances its cursor
        // without ever consuming). Evaluating left-to-right and bailing
        // early is the natural zip semantic.
        auto opt_a = co_await next_a();
        if (!opt_a) co_return std::nullopt;
        auto opt_b = co_await next_b();
        if (!opt_b) co_return std::nullopt;
        co_return std::make_pair(std::move(*opt_a), std::move(*opt_b));
    });
}

/**
 * @brief Create stream from async generator function
 * @tparam F Generator function type
 * @param f Function producing values
 * @return Async stream
 * @ingroup Coroutine
 */
template <typename F>
auto from_generator(F f) -> async_stream<std::invoke_result_t<F>> {
    using T = std::invoke_result_t<F>;

    auto gen = std::make_shared<F>(std::move(f));
    return async_stream<T>([gen]() -> task<std::optional<T>> {
        co_return (*gen)();
    });
}

/**
 * @brief Create stream repeating a value
 * @tparam T Value type
 * @param value Value to repeat
 * @return Infinite stream
 * @ingroup Coroutine
 */
template <typename T>
async_stream<T> repeat_value(T value) {
    auto shared = std::make_shared<T>(std::move(value));
    return async_stream<T>([shared]() -> task<std::optional<T>> {
        co_return *shared;
    });
}

/**
 * @brief Interval stream - emit at fixed intervals
 * @param interval Time between emissions
 * @param start_with_now If true, emit immediately
 * @return Stream of tick counts
 * @ingroup Coroutine
 */
inline async_stream<size_t> interval(
    std::chrono::milliseconds interval_time,
    bool start_with_now = false) {

    auto counter = std::make_shared<size_t>(0);
    auto next_time = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now() + (start_with_now ? std::chrono::milliseconds(0) : interval_time)
    );

    auto interval_ms = interval_time;
    return async_stream<size_t>([counter, next_time, interval_ms]() -> task<std::optional<size_t>> {
        auto now = std::chrono::steady_clock::now();
        if (now < *next_time) {
            co_await sleep(std::chrono::duration_cast<std::chrono::milliseconds>(*next_time - now));
        }

        *next_time += interval_ms;
        co_return (*counter)++;
    });
}

/**
 * @brief Timer stream - emit once after delay
 * @tparam T Value type to emit
 * @param value Value to emit
 * @param delay Delay before emission
 * @return Stream with single value
 * @ingroup Coroutine
 */
template <typename T>
async_stream<T> timer(T value, std::chrono::milliseconds delay) {
    auto shared = std::make_shared<std::optional<T>>(std::move(value));
    auto emitted = std::make_shared<bool>(false);

    return async_stream<T>([shared, emitted, delay]() -> task<std::optional<T>> {
        if (*emitted) co_return std::nullopt;
        *emitted = true;

        co_await sleep(delay);
        co_return std::move(*shared);
    });
}

/**
 * @brief Range stream
 * @tparam T Value type
 * @param start Start value
 * @param end End value (exclusive)
 * @return Stream of range
 * @ingroup Coroutine
 */
template <typename T>
async_stream<T> range_stream(T start, T end) {
    auto current = std::make_shared<T>(start);

    return async_stream<T>([current, end]() -> task<std::optional<T>> {
        if (*current >= end) co_return std::nullopt;
        co_return (*current)++;
    });
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_STREAM_H
