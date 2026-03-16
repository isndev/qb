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
#include <queue>
#include <optional>
#include <vector>

namespace qb::io::async {

/**
 * @brief Exception thrown when trying to use a closed channel
 */
class channel_closed : public std::runtime_error {
public:
    channel_closed() : std::runtime_error("Channel is closed") {}
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

        // No lock: single-thread cooperative — state is stable between
        // await_ready() and await_suspend() (no other coroutine can run).
        bool await_ready() {
            if (ch._closed)
                return false;
            if (!ch._recv_waiters.empty()) {
                auto [recv_h, result_ptr] = ch._recv_waiters.front();
                ch._recv_waiters.pop_front();
                *result_ptr = std::move(value);
                _completed = true;
                schedule_via_current(recv_h);
                return true;
            }
            if (ch._buffer.size() < ch._capacity) {
                ch._buffer.push(std::move(value));
                _completed = true;
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            if (ch._closed) {
                _completed = true;
                schedule_via_current(h);
                return;
            }
            if (!ch._recv_waiters.empty()) {
                auto [recv_h, result_ptr] = ch._recv_waiters.front();
                ch._recv_waiters.pop_front();
                *result_ptr = std::move(value);
                _completed = true;
                schedule_via_current(recv_h);
                schedule_via_current(h);
            } else if (ch._buffer.size() < ch._capacity) {
                ch._buffer.push(std::move(value));
                _completed = true;
                schedule_via_current(h);
            } else {
                ch._send_waiters.push_back(h);
            }
        }

        void await_resume() {
            if (ch._closed && !_completed) {
                throw channel_closed();
            }
        }
    };

    /**
     * @brief Send a value to the channel
     * @param value Value to send
     * @return Awaiter that completes when value is in buffer
     * @throws channel_closed if channel is closed
     */
    send_awaiter send(T value) {
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

        bool await_ready() {
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop();
                if (!ch._send_waiters.empty()) {
                    auto send_h = ch._send_waiters.front();
                    ch._send_waiters.pop_front();
                    schedule_via_current(send_h);
                }
                return true;
            }
            if (ch._closed)
                return true;  // _result stays nullopt → channel closed
            return false;
        }

        void await_suspend(std::coroutine_handle<> h) {
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop();
                if (!ch._send_waiters.empty()) {
                    auto send_h = ch._send_waiters.front();
                    ch._send_waiters.pop_front();
                    schedule_via_current(send_h);
                }
                schedule_via_current(h);
            } else if (ch._closed) {
                schedule_via_current(h);
            } else {
                ch._recv_waiters.push_back({h, &_result});
            }
        }

        std::optional<T> await_resume() {
            return std::move(_result);
        }
    };

    /**
     * @brief Receive a value from the channel
     * @return Awaiter that returns optional value
     *         - Has value: received data
     *         - Empty: channel closed
     */
    recv_awaiter recv() {
        return recv_awaiter{*this, std::nullopt};
    }

    /**
     * @brief Non-blocking try send
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
     * @brief Non-blocking try receive
     * @return Optional value - empty if channel empty or closed with no data
     */
    std::optional<T> try_recv() {
        if (!_buffer.empty()) {
            T value = std::move(_buffer.front());
            _buffer.pop();
            if (!_send_waiters.empty()) {
                auto h = _send_waiters.front();
                _send_waiters.pop_front();
                schedule_via_current(h);
            }
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
        for (auto& [h, result_ptr] : _recv_waiters) {
            (void)result_ptr;
            schedule_via_current(h);
        }
        _recv_waiters.clear();
        for (auto h : _send_waiters)
            schedule_via_current(h);
        _send_waiters.clear();
    }

    bool is_closed() const noexcept { return _closed; }
    size_t size()    const noexcept { return _buffer.size(); }
    size_t capacity()const noexcept { return _capacity; }
    bool   empty()   const noexcept { return _buffer.empty(); }

private:
    size_t _capacity;
    using recv_waiter_entry = std::pair<std::coroutine_handle<>, std::optional<T>*>;

    std::queue<T> _buffer;
    std::deque<std::coroutine_handle<>> _send_waiters;
    std::deque<recv_waiter_entry> _recv_waiters;
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
 * @brief Range-based for loop helper for channels
 *
 * Usage:
 * @code
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
            // This would need to be async - simplified version
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

        T operator*() const {
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

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CHANNEL_H
