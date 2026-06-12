/**
 * @file qb/io/async/coroutine/channel.h
 * @brief MPSC Channel for coroutine communication
 *
 * Channels provide communication between coroutines without explicit locking.
 * They implement a producer/consumer pattern with optional buffering.
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

#ifndef QB_IO_ASYNC_COROUTINE_CHANNEL_H
#define QB_IO_ASYNC_COROUTINE_CHANNEL_H

#include "task.h"
#include "utils.h"
// NOTE: No <mutex> — the channel is used exclusively within a single qb-io
// thread under the cooperative scheduler. "Multi-Producer" here means multiple
// coroutines; they are all on the same thread and never run concurrently.
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qb::io::async {

/**
 * @brief Exception thrown when trying to use a closed channel
 */
class channel_closed : public std::runtime_error {
public:
    channel_closed() : std::runtime_error("Channel is closed") {}
};

// =============================================================================
// channel_select_state — shared coordination state for select()
// =============================================================================

/**
 * @brief Shared state for channel select operations.
 *
 * All participating channels hold a pointer to the same select_state.
 * The first channel to deliver a value calls resolve() which wakes the
 * awaiting coroutine. Subsequent channels find the state already resolved
 * and skip. Lazy cleanup of stale select_waiter_entry items happens inside
 * send_awaiter: resolved entries are discarded when encountered.
 *
 * Single-thread cooperative: no mutex needed.
 */
struct channel_select_state {
    bool   resolved{false};
    size_t winner{0};
    bool   closed{false};  ///< true when the winning channel was closed
    std::any value;
    std::coroutine_handle<> outer{};

    void resolve(size_t idx, std::any v, bool ch_closed = false) {
        if (!resolved) {
            resolved = true;
            winner   = idx;
            value    = std::move(v);
            closed   = ch_closed;
            if (outer) schedule_via_current(std::exchange(outer, {}));
        }
    }
};

/**
 * @brief Multi-Producer Single-Consumer channel
 * @tparam T Type of values passed through the channel
 *
 * Channels enable communication between coroutines without explicit locks.
 * Multiple producers can send values; a single consumer receives them.
 *
 * Single-thread: use within one scheduler/thread; not safe for cross-thread use.
 *
 * Usage:
 * @code
 * channel<int> ch(10);  // Buffered channel, capacity 10
 *
 * // Producer
 * task<void> producer() {
 *     for (int i = 0; i < 100; ++i) {
 *         co_await ch.send(i);  // Blocks if buffer full
 *     }
 *     ch.close();
 * }
 *
 * // Consumer
 * task<void> consumer() {
 *     while (true) {
 *         try {
 *             auto val = co_await ch.recv();
 *             process(val);
 *         } catch (channel_closed&) {
 *             break;
 *         }
 *     }
 * }
 * @endcode
 */
template <typename T>
class channel {
public:
    /**
     * @brief Create a channel with given capacity
     * @param capacity Buffer size (0 = unbuffered, rendezvous mode)
     */
    explicit channel(size_t capacity = 0)
        : _capacity(capacity) {}

    /**
     * @brief Destructor - closes the channel
     */
    ~channel() {
        close();
    }

    // Non-copyable, non-movable
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    /**
     * @brief Awaiter for send operation
     *
     * Fast path: await_ready() returns true when send can complete without
     * suspending (buffer has space or a receiver is waiting).
     */
    struct send_awaiter {
        channel& ch;
        T value;
        bool _completed = false;
        std::coroutine_handle<> _parked{}; ///< set when queued in _send_waiters

        // Explicit constructor: the user-declared destructor below makes this
        // a non-aggregate, so send() needs a constructor for brace-init.
        send_awaiter(channel& c, T v) : ch(c), value(std::move(v)) {}

        // If this sender is still parked when its coroutine frame is destroyed,
        // remove its entry from _send_waiters so a later wake_one_sender()/close()
        // cannot schedule_via_current() a dangling handle (use-after-free).
        ~send_awaiter() {
            if (!_parked)
                return;
            auto& w = ch._send_waiters;
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (it->handle == _parked) {
                    w.erase(it);
                    break;
                }
            }
        }

        // No lock: single-thread cooperative — state is stable between
        // await_ready() and await_suspend() (no other coroutine can run).
        //
        // Closed-channel semantics: a closed channel must **reject** every
        // subsequent `send` by throwing `channel_closed` from `await_resume`.
        // We take the synchronous fast-path (await_ready returns true) so the
        // throw happens without an intermediate suspension — this mirrors
        // `try_send` which returns `false` on a closed channel (Finding 2.C.1).
        [[nodiscard]] bool await_ready() {
            if (ch._closed) {
                // Stay !_completed so await_resume throws channel_closed.
                return true;
            }
            // 1. Satisfy a direct recv waiter
            if (!ch._recv_waiters.empty()) {
                auto [recv_h, result_ptr] = ch._recv_waiters.front();
                ch._recv_waiters.pop_front();
                *result_ptr = std::move(value);
                _completed  = true;
                schedule_via_current(recv_h);
                return true;
            }
            // 2. Satisfy a select waiter (lazy-clean stale resolved entries)
            while (!ch._select_waiters.empty()) {
                auto& entry = ch._select_waiters.front();
                if (!entry.state->resolved) {
                    entry.state->resolve(entry.index, std::any(std::move(value)));
                    ch._select_waiters.pop_front();
                    _completed = true;
                    return true;
                }
                ch._select_waiters.pop_front();  // stale, skip
            }
            // 3. Buffer
            if (ch._buffer.size() < ch._capacity) {
                ch._buffer.push(std::move(value));
                _completed = true;
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            // Channel was closed while we were queued or between await_ready()
            // and await_suspend() (cannot actually happen under the cooperative
            // model since no other code runs, but guard defensively).
            if (ch._closed) {
                // Do NOT mark _completed: await_resume will throw.
                schedule_via_current(h);
                return;
            }
            if (!ch._recv_waiters.empty()) {
                auto [recv_h, result_ptr] = ch._recv_waiters.front();
                ch._recv_waiters.pop_front();
                *result_ptr = std::move(value);
                _completed  = true;
                schedule_via_current(recv_h);
                schedule_via_current(h);
                return;
            }
            while (!ch._select_waiters.empty()) {
                auto& entry = ch._select_waiters.front();
                if (!entry.state->resolved) {
                    entry.state->resolve(entry.index, std::any(std::move(value)));
                    ch._select_waiters.pop_front();
                    _completed = true;
                    schedule_via_current(h);
                    return;
                }
                ch._select_waiters.pop_front();
            }
            if (ch._buffer.size() < ch._capacity) {
                ch._buffer.push(std::move(value));
                _completed = true;
                schedule_via_current(h);
            } else {
                _parked = h;
                ch._send_waiters.push_back({h, nullptr});
            }
        }

        void await_resume() {
            // Finding 2.C.1: after a close() has happened (before or after the
            // suspension), the value must be rejected loudly rather than
            // silently dropped. This matches try_send's boolean-false contract
            // and the Doxygen "@throws channel_closed" on send().
            if (ch._closed && !_completed) {
                throw channel_closed();
            }
            if (!_completed) {
                // Woken by a recv that freed buffer space — deliver our value.
                // If a receiver is already waiting, hand it directly.
                if (!ch._recv_waiters.empty()) {
                    auto [recv_h, result_ptr] = ch._recv_waiters.front();
                    ch._recv_waiters.pop_front();
                    *result_ptr = std::move(value);
                    schedule_via_current(recv_h);
                } else {
                    ch._buffer.push(std::move(value));
                }
                _completed = true;
            }
        }
    };

    /**
     * @brief Send a value to the channel
     * @param value Value to send
     * @return Awaiter that completes when value is in buffer
     * @throws channel_closed if channel is closed
     */
    [[nodiscard]] send_awaiter send(T value) {
        return send_awaiter{*this, std::move(value)};
    }

    /**
     * @brief Awaiter for receive operation
     *
     * Fast path: await_ready() returns true when a value is already available
     * or the channel is closed (receive can complete without suspending).
     */
    struct recv_awaiter {
        channel& ch;
        std::optional<T> _result;

        // Explicit constructor: the user-declared destructor below makes this a
        // non-aggregate, so recv() needs a constructor for brace-init.
        recv_awaiter(channel& c, std::optional<T> r)
            : ch(c), _result(std::move(r)) {}

        // If this receiver is still parked when its coroutine frame is destroyed
        // (e.g. scheduler teardown / a cancelled parent), remove its entry from
        // _recv_waiters so a later send()/try_send() cannot write through the
        // now-dangling &_result into freed memory (use-after-free).
        ~recv_awaiter() {
            auto& w = ch._recv_waiters;
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (it->second == &_result) {
                    w.erase(it);
                    break;
                }
            }
        }

        [[nodiscard]] bool await_ready() {
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop();
                ch.wake_one_sender();
                return true;
            }
            if (ch._closed)
                return true;
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop();
                ch.wake_one_sender();
                schedule_via_current(h);
            } else if (ch._closed) {
                schedule_via_current(h);
            } else {
                ch._recv_waiters.push_back({h, &_result});
                // Rendezvous fix (capacity==0 sender-first):
                // if a sender was already suspended, wake one immediately so it
                // can hand off directly to this newly parked receiver.
                ch.wake_one_sender();
            }
        }

        std::optional<T> await_resume() {
            // If woken by close() while data is still in the buffer, drain it.
            if (!_result && !ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop();
                ch.wake_one_sender();
            }
            return std::move(_result);
        }
    };

    /**
     * @brief Receive a value from the channel
     * @return Awaiter that returns optional value
     *         - Has value: received data
     *         - Empty: channel closed
     */
    [[nodiscard]] recv_awaiter recv() {
        return recv_awaiter{*this, std::nullopt};
    }

    /**
     * @brief Non-blocking try send (copy)
     * @param value Value to send
     * @return true if sent, false if buffer full or closed
     */
    bool try_send(const T& value) {
        if (_closed)
            return false;
        if (!_recv_waiters.empty()) {
            auto [recv_h, result_ptr] = _recv_waiters.front();
            _recv_waiters.pop_front();
            *result_ptr = value;
            schedule_via_current(recv_h);
            return true;
        }
        if (_buffer.size() < _capacity) {
            _buffer.push(value);
            return true;
        }
        return false;
    }

    /**
     * @brief Non-blocking try send (move)
     * @param value Value to send (moved)
     * @return true if sent, false if buffer full or closed
     */
    bool try_send(T&& value) {
        if (_closed)
            return false;
        if (!_recv_waiters.empty()) {
            auto [recv_h, result_ptr] = _recv_waiters.front();
            _recv_waiters.pop_front();
            *result_ptr = std::move(value);
            schedule_via_current(recv_h);
            return true;
        }
        if (_buffer.size() < _capacity) {
            _buffer.push(std::move(value));
            return true;
        }
        return false;
    }

    /**
     * @brief Non-blocking try receive
     * @return Optional value - empty if channel empty or closed with no data
     */
    std::optional<T> try_recv() {
        if (!_buffer.empty()) {
            T value = std::move(_buffer.front());
            _buffer.pop();
            wake_one_sender();
            return value;
        }
        return std::nullopt;
    }

    /**
     * @brief Close the channel
     *
     * No more sends allowed. Pending receives get empty optionals.
     * Pending sends get channel_closed exception.
     */
    void close() {
        if (_closed) return;
        _closed = true;
        for (auto& [h, result_ptr] : _recv_waiters) { (void)result_ptr; schedule_via_current(h); }
        _recv_waiters.clear();
        for (auto& entry : _send_waiters) {
            if (entry.guard && *entry.guard) continue;
            if (entry.guard) *entry.guard = true;
            schedule_via_current(entry.handle);
        }
        _send_waiters.clear();
        // Notify select waiters with "closed" signal
        for (auto& entry : _select_waiters)
            entry.state->resolve(entry.index, std::any{}, /*closed=*/true);
        _select_waiters.clear();
    }

    bool is_closed() const noexcept { return _closed; }
    size_t size()    const noexcept { return _buffer.size(); }
    size_t capacity()const noexcept { return _capacity; }
    bool   empty()   const noexcept { return _buffer.empty(); }

    // -----------------------------------------------------------------------
    // select() support
    // -----------------------------------------------------------------------

    /**
     * @brief Register a select operation as a potential receiver on this channel.
     *
     * Called by channel_select_awaiter before suspending. When a value becomes
     * available (via send or close), this channel will call state->resolve() if
     * the select is not yet resolved.
     */
    void register_select_waiter(std::shared_ptr<channel_select_state> state, size_t idx) {
        // Fast path: value already in buffer
        if (!_buffer.empty()) {
            T val = std::move(_buffer.front()); _buffer.pop();
            wake_one_sender();
            state->resolve(idx, std::any(std::move(val)));
            return;
        }
        if (_closed) {
            state->resolve(idx, std::any{}, true);
            return;
        }
        _select_waiters.push_back({std::move(state), idx});
    }

    // -----------------------------------------------------------------------
    // Timed recv / send
    // -----------------------------------------------------------------------

    /**
     * @brief Receive with timeout
     * @param timeout Maximum time to wait
     * @return Optional value — empty on timeout or closed channel
     */
    task<std::optional<T>> recv_for(std::chrono::milliseconds timeout) {
        // Fast path
        if (!_buffer.empty()) {
            co_return try_recv();
        }
        if (_closed) co_return std::nullopt;

        auto state = std::make_shared<channel_select_state>();
        // index 0 = data received, resolved via timer with winner=1 = timeout
        struct timed_recv_awaiter {
            channel<T>& ch;
            std::shared_ptr<channel_select_state> state;
            std::chrono::milliseconds timeout_ms;

            timed_recv_awaiter(channel<T>& c,
                               std::shared_ptr<channel_select_state> s,
                               std::chrono::milliseconds t)
                : ch(c), state(std::move(s)), timeout_ms(t) {}

            // Frame-destruction guard: this awaiter lives inside the recv_for
            // coroutine frame. If that frame is destroyed while parked, mark the
            // shared state resolved so neither channel_timer nor a later sender
            // schedules the now-dangling handle.
            ~timed_recv_awaiter() {
                if (state && !state->resolved) {
                    state->resolved = true;
                    state->outer    = {};
                }
            }

            [[nodiscard]] bool await_ready() const noexcept { return state->resolved; }

            void await_suspend(std::coroutine_handle<> h) {
                if (state->resolved) { schedule_via_current(h); return; }
                state->outer = h;
                ch._select_waiters.push_back({state, 0});
                coro_scheduler().spawn(channel_timer(state, h, timeout_ms));
            }

            std::optional<T> await_resume() {
                if (!state->resolved || state->winner == 1 || state->closed)
                    return std::nullopt;
                if (!state->value.has_value()) return std::nullopt;
                return std::any_cast<T>(std::move(state->value));
            }
        };

        co_return co_await timed_recv_awaiter{*this, state, timeout};
    }

    /**
     * @brief Send with timeout
     * @param value  Value to send
     * @param timeout Maximum time to wait for buffer space
     * @return true if sent, false on timeout or closed channel
     */
    task<bool> send_for(T value, std::chrono::milliseconds timeout) {
        if (_closed) co_return false;
        // Fast path: space available
        if (_buffer.size() < _capacity || !_recv_waiters.empty()) {
            co_await send(std::move(value));
            co_return true;
        }

        // Slow path: wait for space or timeout.
        // guard: shared flag — whichever wakes us first (recv or timer) sets it
        // to true, preventing the other from double-scheduling the handle.
        auto guard = std::make_shared<bool>(false);
        auto fired = std::make_shared<bool>(false);

        struct timed_send_awaiter {
            channel<T>& ch;
            T val;
            std::shared_ptr<bool> guard;
            std::shared_ptr<bool> fired;
            std::chrono::milliseconds timeout_ms;
            bool _resumed = false;

            timed_send_awaiter(channel<T>& c, T v, std::shared_ptr<bool> g,
                               std::shared_ptr<bool> f, std::chrono::milliseconds t)
                : ch(c), val(std::move(v)), guard(std::move(g)),
                  fired(std::move(f)), timeout_ms(t) {}

            // Frame-destruction guard: if the send_for frame is destroyed while
            // parked, set *guard so neither send_timer nor wake_one_sender()
            // schedules the dangling handle (the stale _send_waiters entry is
            // lazily discarded by the guard check).
            ~timed_send_awaiter() {
                if (!_resumed && guard && !*guard)
                    *guard = true;
            }

            [[nodiscard]] bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                ch._send_waiters.push_back(send_waiter_entry{h, guard});
                coro_scheduler().spawn(send_timer(guard, fired, h, timeout_ms));
            }

            bool await_resume() {
                _resumed = true; // normal resume path: destructor must not re-arm guard
                if (*fired) return false;
                if (ch._closed) return false;
                // Deliver directly to a pending receiver if one exists
                if (!ch._recv_waiters.empty()) {
                    auto [recv_h, result_ptr] = ch._recv_waiters.front();
                    ch._recv_waiters.pop_front();
                    *result_ptr = std::move(val);
                    schedule_via_current(recv_h);
                } else {
                    ch._buffer.push(std::move(val));
                }
                return true;
            }
        };

        co_return co_await timed_send_awaiter{*this, std::move(value), guard, fired, timeout};
    }

private:
    // Free functions: parameters stored by value in the coroutine frame.
    static task<void> channel_timer(std::shared_ptr<channel_select_state> state,
                                     std::coroutine_handle<> h,
                                     std::chrono::milliseconds delay) {
        co_await sleep(delay);
        // winner=1 signals timeout; schedule_via_current deduplicates
        if (!state->resolved) {
            state->resolved = true;
            state->winner   = 1;
            schedule_via_current(h);
        }
    }

    static task<void> send_timer(std::shared_ptr<bool> guard,
                                  std::shared_ptr<bool> fired,
                                  std::coroutine_handle<> h,
                                  std::chrono::milliseconds delay) {
        co_await sleep(delay);
        if (!*guard) {
            *guard = true;
            *fired = true;
            schedule_via_current(h);
        }
    }

    size_t _capacity;
    using recv_waiter_entry = std::pair<std::coroutine_handle<>, std::optional<T>*>;

    struct send_waiter_entry {
        std::coroutine_handle<> handle;
        std::shared_ptr<bool>   guard;  // set to true on wake (prevents timer double-schedule)
    };

    struct select_waiter_entry {
        std::shared_ptr<channel_select_state> state;
        size_t index;
    };

    void wake_one_sender() {
        while (!_send_waiters.empty()) {
            auto entry = _send_waiters.front();
            _send_waiters.pop_front();
            if (entry.guard && *entry.guard) continue;  // already woken by timer
            if (entry.guard) *entry.guard = true;
            schedule_via_current(entry.handle);
            return;
        }
    }

    std::queue<T>  _buffer;
    std::deque<send_waiter_entry>       _send_waiters;
    std::deque<recv_waiter_entry>       _recv_waiters;
    std::deque<select_waiter_entry>     _select_waiters;
    bool _closed = false;
};

/**
 * @brief Helper to create a channel
 * @tparam T Value type
 * @param capacity Channel buffer capacity
 * @return Unique pointer to new channel
 * @ingroup Coroutine
 */
template <typename T>
auto make_channel(size_t capacity = 0) {
    return std::make_unique<channel<T>>(capacity);
}

/**
 * @brief Range-based for loop helper for channels — **non-blocking** drain
 *
 * Finding 2.C.9 — scope & naming:
 *   This helper does **not** suspend: it calls `try_recv()` and stops at
 *   the first empty slot, so it is really a "drain everything currently
 *   buffered" loop, not a coroutine-aware iteration. It cannot be used
 *   inside a `for (auto v : channel_range(ch))` to wait for new items;
 *   for that use `async_stream::from_channel(ch)` with a `while (auto v
 *   = co_await stream._next())` pattern, or iterate manually with
 *   `co_await ch.recv()`.
 *
 *   `operator*` returns by value (moved out of the iterator's private
 *   optional). This is intentional: the iterator owns the value until
 *   `operator++` overwrites it, so moving into the loop variable is
 *   safe and cheaper than a copy. The method is no longer `const` to
 *   reflect the logical ownership transfer.
 *
 * Usage:
 * @code
 * // Only drains what is already buffered; stops on the first empty slot.
 * for (auto val : channel_range(ch)) {
 *     process(val);
 * }
 * @endcode
 */
template <typename T>
class channel_range {
    channel<T>& _ch;

public:
    explicit channel_range(channel<T>& ch) : _ch(ch) {}

    class iterator {
        channel<T>& _ch;
        std::optional<T> _current;
        bool _done = false;

    public:
        iterator(channel<T>& ch, bool end = false) : _ch(ch), _done(end) {
            if (!end) {
                advance();
            }
        }

        void advance() {
            // Non-blocking: see class doc. Use async_stream for true async
            // iteration.
            _current = _ch.try_recv();
            if (!_current) {
                _done = true;
            }
        }

        bool operator!=(const iterator& other) const {
            return _done != other._done;
        }

        iterator& operator++() {
            advance();
            return *this;
        }

        T operator*() {
            return std::move(*_current);
        }
    };

    iterator begin() { return iterator{_ch}; }
    iterator end() { return iterator{_ch, true}; }
};

/**
 * @brief Transform values from one channel to another
 * @tparam T Input type
 * @tparam U Output type
 * @param in Input channel
 * @param out Output channel
 * @param f Transform function
 * @return Task that runs until input closed
 * @ingroup Coroutine
 */
template <typename T, typename U, typename F>
task<void> transform(channel<T>& in, channel<U>& out, F f) {
    while (true) {
        auto val = co_await in.recv();
        if (!val) break;

        co_await out.send(f(*val));
    }
    out.close();
}

/**
 * @brief Filter values from one channel to another
 * @tparam T Value type
 * @param in Input channel
 * @param out Output channel
 * @param pred Predicate function
 * @return Task that runs until input closed
 * @ingroup Coroutine
 */
template <typename T, typename F>
task<void> filter(channel<T>& in, channel<T>& out, F pred) {
    while (true) {
        auto val = co_await in.recv();
        if (!val) break;

        if (pred(*val)) {
            co_await out.send(*val);
        }
    }
    out.close();
}

/**
 * @brief Collect all values from channel into vector
 * @tparam T Value type
 * @param ch Channel to drain
 * @return Task returning vector of all values
 * @ingroup Coroutine
 */
template <typename T>
task<std::vector<T>> collect(channel<T>& ch) {
    std::vector<T> result;

    while (true) {
        auto val = co_await ch.recv();
        if (!val) break;
        result.push_back(std::move(*val));
    }

    co_return result;
}

/**
 * @brief Create producer-consumer pipeline
 *
 * Usage:
 * @code
 * auto [in, out] = make_pipeline<T, U>([](T val) -> U {
 *     return process(val);
 * }, buffer_size);
 * @endcode
 *
 * NOTE: The caller must keep the returned channels alive while the pipeline
 * worker is running. The worker holds raw pointers to the channels.
 */
template <typename T, typename U, typename F>
auto make_pipeline(F f, size_t buffer_size = 10) {
    auto in  = std::make_unique<channel<T>>(buffer_size);
    auto out = std::make_unique<channel<U>>(buffer_size);

    // Capture raw pointers BEFORE moving the unique_ptrs.
    // Use a static function (not a lambda) so the coroutine frame stores fn,
    // in_ptr, out_ptr as VALUE parameters — never as a pointer to a local lambda
    // that would dangle after make_pipeline returns.
    coro_scheduler().spawn(pipeline_worker<T, U>(std::move(f), in.get(), out.get()));

    return std::make_pair(std::move(in), std::move(out));
}

// Free function: fn, in_ptr, out_ptr are VALUE/pointer parameters stored
// in the coroutine frame by the standard. No local-lambda dangling risk.
template <typename T, typename U, typename F>
task<void> pipeline_worker(F fn, channel<T>* in_ptr, channel<U>* out_ptr) {
    while (true) {
        auto val = co_await in_ptr->recv();
        if (!val) break;
        co_await out_ptr->send(fn(*val));
    }
    out_ptr->close();
}

// =============================================================================
// select() — wait on the first of N channels to have data
// =============================================================================

/**
 * @brief Result of a select() operation
 *
 * - index  : which channel (0-based) won the race
 * - closed : true if the winning channel was closed (value is empty)
 * - value  : std::any wrapping the received value (empty on close/timeout)
 */
struct select_result {
    size_t   index{0};
    bool     closed{false};
    std::any value;

    template <typename T>
    T get() const { return std::any_cast<T>(value); }
};

/**
 * @brief Awaiter for variadic heterogeneous channel select
 * @tparam Ts  Value types of the participated channels
 *
 * Registers with every channel as a select waiter. The first channel to
 * deliver a value (or close) resolves the shared state and wakes the caller.
 * Stale entries in other channels' select queues are cleaned up lazily.
 */
template <typename... Ts>
class channel_select_awaiter {
    using state_t = channel_select_state;
    std::shared_ptr<state_t>     _state;
    std::tuple<channel<Ts>*...>  _channels;

    // Pass 1: prefer any channel that has buffered data.
    template <size_t I>
    bool try_data() {
        auto* ch = std::get<I>(_channels);
        if (!ch->empty()) {
            if (auto val = ch->try_recv()) {
                _state->resolve(I, std::any(std::move(*val)));
                return true;
            }
        }
        if constexpr (I + 1 < sizeof...(Ts))
            return try_data<I + 1>();
        return false;
    }

    // Pass 2: only after no data anywhere, report a closed channel.
    template <size_t I>
    bool try_closed() {
        auto* ch = std::get<I>(_channels);
        if (ch->is_closed() && ch->empty()) {
            _state->resolve(I, std::any{}, true);
            return true;
        }
        if constexpr (I + 1 < sizeof...(Ts))
            return try_closed<I + 1>();
        return false;
    }

    bool try_immediate() { return try_data<0>() || try_closed<0>(); }

    template <size_t... Is>
    void register_all(std::index_sequence<Is...>) {
        (std::get<Is>(_channels)->register_select_waiter(_state, Is), ...);
    }

public:
    explicit channel_select_awaiter(channel<Ts>&... chs)
        : _state(std::make_shared<state_t>())
        , _channels(&chs...) {}

    [[nodiscard]] bool await_ready() { return try_immediate(); }

    void await_suspend(std::coroutine_handle<> h) {
        if (_state->resolved) { schedule_via_current(h); return; }
        _state->outer = h;
        register_all(std::make_index_sequence<sizeof...(Ts)>{});
    }

    select_result await_resume() {
        return {_state->winner, _state->closed, std::move(_state->value)};
    }
};

/**
 * @brief Wait for the first of several channels to have data (variadic)
 *
 * @code
 * auto res = co_await select(ch_int, ch_string);
 * if (res.index == 0) use(res.get<int>());
 * else               use(res.get<std::string>());
 * @endcode
 *
 * @ingroup Coroutine
 */
template <typename... Ts>
auto select(channel<Ts>&... chs) {
    return channel_select_awaiter<Ts...>(chs...);
}

/**
 * @brief Homogeneous vector-of-channels select
 *
 * Waits for the first channel in @p channels to deliver a value.
 * Returns {index, closed, any(value)}.
 *
 * @ingroup Coroutine
 */
template <typename T>
class channel_select_vector_awaiter {
    using state_t = channel_select_state;
    std::shared_ptr<state_t>  _state;
    std::vector<channel<T>*>  _channels;

public:
    explicit channel_select_vector_awaiter(std::vector<channel<T>*> chs)
        : _state(std::make_shared<state_t>())
        , _channels(std::move(chs)) {}

    [[nodiscard]] bool await_ready() {
        // Pass 1: prefer data over close signals (avoids starvation).
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto* ch = _channels[i];
            if (!ch->empty()) {
                auto val = ch->try_recv();
                if (val) { _state->resolve(i, std::any(std::move(*val))); return true; }
            }
        }
        // Pass 2: report first closed-and-empty channel.
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto* ch = _channels[i];
            if (ch->is_closed() && ch->empty()) {
                _state->resolve(i, std::any{}, true);
                return true;
            }
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        if (_state->resolved) { schedule_via_current(h); return; }
        _state->outer = h;
        for (size_t i = 0; i < _channels.size(); ++i)
            _channels[i]->register_select_waiter(_state, i);
    }

    select_result await_resume() {
        return {_state->winner, _state->closed, std::move(_state->value)};
    }
};

template <typename T>
auto select(std::vector<channel<T>*> chs) {
    return channel_select_vector_awaiter<T>(std::move(chs));
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CHANNEL_H
