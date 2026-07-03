/**
 * @file qb/io/async/coroutine/channel.h
 * @brief MPSC Channel for coroutine communication
 *
 * Channels provide communication between coroutines without explicit locking.
 * They implement a producer/consumer pattern with optional buffering.
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

#ifndef QB_IO_ASYNC_COROUTINE_CHANNEL_H
#define QB_IO_ASYNC_COROUTINE_CHANNEL_H

#include "task.h"
#include "utils.h"
// NOTE: No <mutex> — the channel is used exclusively within a single qb-io
// thread under the cooperative scheduler. "Multi-Producer" here means multiple
// coroutines; they are all on the same thread and never run concurrently.
#include <qb/system/time.h> // qb::duration
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
    channel_closed()
        : std::runtime_error("Channel is closed") {}
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
    bool                    resolved{false};
    size_t                  winner{0};
    bool                    closed{false}; ///< true when the winning channel was closed
    std::any                value;
    std::coroutine_handle<> outer{};

    void
    resolve(size_t idx, std::any v, bool ch_closed = false) {
        if (!resolved) {
            resolved = true;
            winner   = idx;
            value    = std::move(v);
            closed   = ch_closed;
            if (outer)
                schedule_via_current(std::exchange(outer, {}));
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
        // Mark dead BEFORE close(): close() schedules deferred resumes of parked
        // waiters; once this destructor returns the channel is freed, so those resumes
        // (and the awaiters' own destructors) must see _alive == false and skip every
        // access to this channel.
        *_alive = false;
        close();
    }

    // Non-copyable, non-movable
    channel(const channel &)            = delete;
    channel &operator=(const channel &) = delete;
    channel(channel &&)                 = delete;
    channel &operator=(channel &&)      = delete;

    /**
     * @brief Awaiter for send operation
     *
     * Fast path: await_ready() returns true when send can complete without
     * suspending (buffer has space or a receiver is waiting).
     */
    struct send_awaiter {
        channel                &ch;
        T                       value;
        bool                    _completed = false;
        std::coroutine_handle<> _parked{}; ///< set when queued in _send_waiters
        std::shared_ptr<bool>   _ch_alive; ///< channel liveness; skip ch access when false

        // Explicit constructor: the user-declared destructor below makes this
        // a non-aggregate, so send() needs a constructor for brace-init.
        send_awaiter(channel &c, T v)
            : ch(c)
            , value(std::move(v))
            , _ch_alive(c._alive) {}

        // If this sender is still parked when its coroutine frame is destroyed,
        // remove its entry from _send_waiters so a later wake_one_sender()/close()
        // cannot schedule_via_current() a dangling handle (use-after-free).
        ~send_awaiter() {
            if (!_parked || !*_ch_alive) // channel already freed: nothing to de-register
                return;
            auto &w = ch._send_waiters;
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
        [[nodiscard]] bool
        await_ready() {
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
                auto &entry = ch._select_waiters.front();
                if (!entry.state->resolved) {
                    entry.state->resolve(entry.index, std::any(std::move(value)));
                    ch._select_waiters.pop_front();
                    _completed = true;
                    return true;
                }
                ch._select_waiters.pop_front(); // stale, skip
            }
            // 3. Buffer
            if (ch._buffer.size() < ch._capacity) {
                ch._buffer.push(std::move(value));
                _completed = true;
                return true;
            }
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
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
                auto &entry = ch._select_waiters.front();
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

        void
        await_resume() {
            // The channel was destroyed while we were parked: the send could not be
            // delivered, so fail it the same way a close() does — without touching the
            // freed channel.
            if (!*_ch_alive)
                throw channel_closed();
            // Finding 2.C.1: after a close() has happened (before or after the
            // suspension), the value must be rejected loudly rather than
            // silently dropped. This matches try_send's boolean-false contract
            // and the Doxygen "@throws channel_closed" on send().
            if (ch._closed && !_completed) {
                throw channel_closed();
            }
            if (!_completed) {
                // Woken by a recv that freed buffer space, or by a select/recv_for that registered
                // after we parked — deliver our value with the same priority as a direct send:
                // a waiting receiver first, then a waiting select waiter, else buffer it.
                if (!ch._recv_waiters.empty()) {
                    auto [recv_h, result_ptr] = ch._recv_waiters.front();
                    ch._recv_waiters.pop_front();
                    *result_ptr = std::move(value);
                    schedule_via_current(recv_h);
                    _completed = true;
                } else {
                    // Hand off to the first non-stale select waiter (resolve() schedules its outer).
                    while (!ch._select_waiters.empty()) {
                        auto entry = ch._select_waiters.front();
                        ch._select_waiters.pop_front();
                        if (!entry.state->resolved) {
                            entry.state->resolve(entry.index, std::any(std::move(value)));
                            _completed = true;
                            break;
                        }
                    }
                    if (!_completed) {
                        ch._buffer.push(std::move(value));
                        _completed = true;
                    }
                }
            }
        }
    };

    /**
     * @brief Send a value to the channel
     * @param value Value to send
     * @return Awaiter that completes when value is in buffer
     * @throws channel_closed if channel is closed
     */
    [[nodiscard]] send_awaiter
    send(T value) {
        return send_awaiter{*this, std::move(value)};
    }

    /**
     * @brief Awaiter for receive operation
     *
     * Fast path: await_ready() returns true when a value is already available
     * or the channel is closed (receive can complete without suspending).
     */
    struct recv_awaiter {
        channel              &ch;
        std::optional<T>      _result;
        bool                  _resumed = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::shared_ptr<bool> _ch_alive;        ///< channel liveness; skip ch access when false

        // Explicit constructor: the user-declared destructor below makes this a
        // non-aggregate, so recv() needs a constructor for brace-init.
        recv_awaiter(channel &c, std::optional<T> r)
            : ch(c)
            , _result(std::move(r))
            , _ch_alive(c._alive) {}

        // If this receiver is still parked when its coroutine frame is destroyed
        // (e.g. scheduler teardown / a cancelled parent), remove its entry from
        // _recv_waiters so a later send()/try_send() cannot write through the
        // now-dangling &_result into freed memory (use-after-free).
        //
        // Wake-token preservation: a sender (send_awaiter / try_send) hands its value by writing
        // *result_ptr (our _result) and POPPING us from _recv_waiters. If we are reclaimed before
        // resuming (a when_any/race loser, woken-then-reclaimed), that value would be destroyed with
        // the frame — a LOST MESSAGE, though the sender reported success. Detect it (we are no longer
        // in _recv_waiters AND never resumed AND _result holds a value) and re-buffer it so the next
        // receiver gets it. Gated on !_resumed: on the normal path await_resume std::move's _result
        // (which leaves the optional engaged-but-moved-from), so without the gate we would re-buffer a
        // moved-from value (double/garbage delivery).
        ~recv_awaiter() {
            if (!*_ch_alive)
                return; // channel already freed: nothing to de-register
            auto &w = ch._recv_waiters;
            for (auto it = w.begin(); it != w.end(); ++it) {
                if (it->second == &_result) {
                    w.erase(it); // still parked: no value handed yet, just retract
                    return;
                }
            }
            // Not in the queue ⇒ a sender/buffer-drain already handed us a value. Reclaimed before
            // consuming it → return it to the channel so the message is not lost.
            if (!_resumed && _result.has_value())
                ch._buffer.push(std::move(*_result));
        }

        [[nodiscard]] bool
        await_ready() {
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

        void
        await_suspend(std::coroutine_handle<> h) {
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

        std::optional<T>
        await_resume() {
            _resumed = true; // consumed normally → the dtor must not re-buffer the moved-from _result
            // The channel was destroyed while we were parked (close() scheduled this
            // resume, ~channel then freed the channel): return nullopt — the documented
            // "channel closed" result — without touching the freed channel.
            if (!*_ch_alive)
                return std::move(_result);
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
    [[nodiscard]] recv_awaiter
    recv() {
        return recv_awaiter{*this, std::nullopt};
    }

    /**
     * @brief Non-blocking try send (copy)
     * @param value Value to send
     * @return true if sent, false if buffer full or closed
     */
    bool
    try_send(const T &value) {
        if (_closed)
            return false;
        if (!_recv_waiters.empty()) {
            auto [recv_h, result_ptr] = _recv_waiters.front();
            _recv_waiters.pop_front();
            *result_ptr = value;
            schedule_via_current(recv_h);
            return true;
        }
        // Satisfy a parked select()/recv_for() waiter before the buffer — mirrors send_awaiter.
        // Without this, try_send returned false on a cap-0 channel (or a full buffer) even when a
        // select waiter was ready, deadlocking a rendezvous (the send path already handles both).
        while (!_select_waiters.empty()) {
            auto &entry = _select_waiters.front();
            if (!entry.state->resolved) {
                entry.state->resolve(entry.index, std::any(value));
                _select_waiters.pop_front();
                return true;
            }
            _select_waiters.pop_front(); // stale, skip
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
    bool
    try_send(T &&value) {
        if (_closed)
            return false;
        if (!_recv_waiters.empty()) {
            auto [recv_h, result_ptr] = _recv_waiters.front();
            _recv_waiters.pop_front();
            *result_ptr = std::move(value);
            schedule_via_current(recv_h);
            return true;
        }
        // Satisfy a parked select()/recv_for() waiter before the buffer — mirrors send_awaiter.
        // Guarded: select() type-erases through std::any (copy-constructible only), so a move-only
        // channel can never hold a select waiter — the block is dead there and must not instantiate.
        if constexpr (std::is_copy_constructible_v<T>) {
            while (!_select_waiters.empty()) {
                auto &entry = _select_waiters.front();
                if (!entry.state->resolved) {
                    entry.state->resolve(entry.index, std::any(std::move(value)));
                    _select_waiters.pop_front();
                    return true;
                }
                _select_waiters.pop_front(); // stale, skip
            }
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
    std::optional<T>
    try_recv() {
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
    void
    close() {
        if (_closed)
            return;
        _closed = true;
        for (auto &[h, result_ptr] : _recv_waiters) {
            (void) result_ptr;
            schedule_via_current(h);
        }
        _recv_waiters.clear();
        for (auto &entry : _send_waiters) {
            if (entry.guard && *entry.guard)
                continue;
            if (entry.guard)
                *entry.guard = true;
            schedule_via_current(entry.handle);
        }
        _send_waiters.clear();
        // Notify select waiters with "closed" signal
        for (auto &entry : _select_waiters)
            entry.state->resolve(entry.index, std::any{}, /*closed=*/true);
        _select_waiters.clear();
    }

    bool
    is_closed() const noexcept {
        return _closed;
    }
    size_t
    size() const noexcept {
        return _buffer.size();
    }
    size_t
    capacity() const noexcept {
        return _capacity;
    }
    bool
    empty() const noexcept {
        return _buffer.empty();
    }

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
    void
    register_select_waiter(std::shared_ptr<channel_select_state> state, size_t idx) {
        // Fast path: value already in buffer
        if (!_buffer.empty()) {
            T val = std::move(_buffer.front());
            _buffer.pop();
            wake_one_sender();
            state->resolve(idx, std::any(std::move(val)));
            return;
        }
        if (_closed) {
            state->resolve(idx, std::any{}, true);
            return;
        }
        _select_waiters.push_back({std::move(state), idx});
        // Rendezvous (capacity==0) sender-first: if a sender is already parked with a value, wake one
        // so its await_resume hands the value to this select/recv_for waiter. Without this, select()
        // and recv_for() never observe a pending rendezvous value that recv()/try_recv() would (the
        // send_awaiter only delivers to recv_waiters on a direct send, not to a select registered
        // afterwards). Mirrors recv_awaiter::await_suspend.
        wake_one_sender();
    }

    /**
     * @brief True iff a `select()` / `recv_for()` waiter is currently parked and
     *        unresolved — i.e. `send()`'s fast path can hand it a value right now.
     * @details `send_for()` must consult this (not just `_recv_waiters` / buffer
     *          space): a `recv_for()` parks in `_select_waiters`, not
     *          `_recv_waiters`, so a `send_for()` that ignored it would take the
     *          slow path and park with a timer while a ready rendezvous partner
     *          waits — both then time out. Stale (resolved) entries are ignored so
     *          a channel littered with them still routes through the timed path.
     */
    [[nodiscard]] bool
    has_live_select_waiter() const noexcept {
        for (const auto &entry : _select_waiters)
            if (entry.state && !entry.state->resolved)
                return true;
        return false;
    }

    // -----------------------------------------------------------------------
    // Timed recv / send
    // -----------------------------------------------------------------------

    /**
     * @brief Receive with timeout
     * @param timeout Maximum time to wait
     * @return Optional value — empty on timeout or closed channel
     */
    task<std::optional<T>>
    recv_for(qb::duration timeout) {
        // Fast path
        if (!_buffer.empty()) {
            co_return try_recv();
        }
        if (_closed)
            co_return std::nullopt;

        auto state = std::make_shared<channel_select_state>();
        // index 0 = data received, resolved via timer with winner=1 = timeout
        struct timed_recv_awaiter {
            channel<T>                           &ch;
            std::shared_ptr<channel_select_state> state;
            qb::duration                          timeout_ms;
            bool                                  _resumed = false; ///< distinguishes resolved-then-reclaimed
            std::shared_ptr<bool>                 _ch_alive;        ///< channel liveness; skip ch access when false

            timed_recv_awaiter(channel<T> &c, std::shared_ptr<channel_select_state> s, qb::duration t)
                : ch(c)
                , state(std::move(s))
                , timeout_ms(t)
                , _ch_alive(c._alive) {}

            // Frame-destruction guard: this awaiter lives inside the recv_for
            // coroutine frame. If that frame is destroyed while parked, mark the
            // shared state resolved so neither channel_timer nor a later sender
            // schedules the now-dangling handle.
            //
            // Wake-token preservation (mirrors recv_awaiter): if a sender already RESOLVED us with a
            // value (state->resolved, not a timeout/close) but we are reclaimed before consuming it,
            // re-buffer that value so the sender's message is not lost. Guarded by _ch_alive (the
            // channel may be torn down first) and !_resumed (await_resume already took the value).
            ~timed_recv_awaiter() {
                if (state && !state->resolved) {
                    state->resolved = true;
                    state->outer    = {};
                    return;
                }
                if (!_resumed && state && _ch_alive && *_ch_alive && state->winner != 1 && !state->closed && state->value.has_value())
                    ch._buffer.push(std::any_cast<T>(std::move(state->value)));
            }

            [[nodiscard]] bool
            await_ready() const noexcept {
                return state->resolved;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                if (state->resolved) {
                    schedule_via_current(h);
                    return;
                }
                state->outer = h;
                ch._select_waiters.push_back({state, 0});
                // Rendezvous (capacity==0) sender-first: wake a parked sender so it delivers to this
                // recv_for waiter (its await_resume now hands off to select waiters). Mirrors
                // register_select_waiter / recv_awaiter::await_suspend.
                ch.wake_one_sender();
                coro_scheduler().spawn(channel_timer(state, h, timeout_ms));
            }

            std::optional<T>
            await_resume() {
                _resumed = true; // consumed normally → the dtor must not re-buffer the value
                if (!state->resolved || state->winner == 1 || state->closed)
                    return std::nullopt;
                if (!state->value.has_value())
                    return std::nullopt;
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
    task<bool>
    send_for(T value, qb::duration timeout) {
        if (_closed)
            co_return false;
        // Fast path: a receiver, a live select()/recv_for() waiter, or buffer
        // space can take the value synchronously. send()'s await_ready performs
        // the actual recv/select/buffer hand-off (and lazily drops stale select
        // entries). The has_live_select_waiter() term is the fix for send_for
        // parking with a timer while a ready recv_for/select partner waits — on a
        // rendezvous channel both would otherwise time out despite a match.
        if (_buffer.size() < _capacity || !_recv_waiters.empty() || has_live_select_waiter()) {
            co_await send(std::move(value));
            co_return true;
        }

        // Slow path: wait for space or timeout.
        // guard: shared flag — whichever wakes us first (recv or timer) sets it
        // to true, preventing the other from double-scheduling the handle.
        auto guard = std::make_shared<bool>(false);
        auto fired = std::make_shared<bool>(false);

        struct timed_send_awaiter {
            channel<T>           &ch;
            T                     val;
            std::shared_ptr<bool> guard;
            std::shared_ptr<bool> fired;
            qb::duration          timeout_ms;
            std::shared_ptr<bool> _ch_alive; ///< channel liveness; skip ch access when false
            bool                  _resumed = false;

            timed_send_awaiter(channel<T> &c, T v, std::shared_ptr<bool> g, std::shared_ptr<bool> f, qb::duration t)
                : ch(c)
                , val(std::move(v))
                , guard(std::move(g))
                , fired(std::move(f))
                , timeout_ms(t)
                , _ch_alive(c._alive) {}

            // Frame-destruction guard: if the send_for frame is destroyed while
            // parked, set *guard so neither send_timer nor wake_one_sender()
            // schedules the dangling handle (the stale _send_waiters entry is
            // lazily discarded by the guard check).
            ~timed_send_awaiter() {
                if (!_resumed && guard && !*guard)
                    *guard = true;
            }

            [[nodiscard]] bool
            await_ready() const noexcept {
                return false;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                ch._send_waiters.push_back(send_waiter_entry{h, guard});
                coro_scheduler().spawn(send_timer(guard, fired, h, timeout_ms));
            }

            bool
            await_resume() {
                _resumed = true; // normal resume path: destructor must not re-arm guard
                // The channel was destroyed while we were parked: close() scheduled
                // this resume, then ~channel freed the channel. The send could not be
                // delivered, so report failure without touching the freed channel
                // (mirrors send_awaiter / recv_awaiter's _ch_alive guard).
                if (!*_ch_alive)
                    return false;
                if (*fired)
                    return false;
                if (ch._closed)
                    return false;
                // Deliver with the SAME priority as a direct/woken untimed send (send_awaiter):
                // a waiting receiver first, then a waiting select/recv_for waiter, else buffer.
                // The select hand-off is essential: a select()/recv_for() that woke us via
                // wake_one_sender() registered in _select_waiters, NOT _recv_waiters. Without it
                // the value would land in the buffer, the select/recv_for waiter would never observe
                // it and would time out — a silently lost rendezvous message. Mirrors
                // send_awaiter::await_resume.
                if (!ch._recv_waiters.empty()) {
                    auto [recv_h, result_ptr] = ch._recv_waiters.front();
                    ch._recv_waiters.pop_front();
                    *result_ptr = std::move(val);
                    schedule_via_current(recv_h);
                    return true;
                }
                // Hand off to the first non-stale select waiter (resolve() schedules its outer).
                while (!ch._select_waiters.empty()) {
                    auto entry = ch._select_waiters.front();
                    ch._select_waiters.pop_front();
                    if (!entry.state->resolved) {
                        entry.state->resolve(entry.index, std::any(std::move(val)));
                        return true;
                    }
                }
                ch._buffer.push(std::move(val));
                return true;
            }
        };

        co_return co_await timed_send_awaiter{*this, std::move(value), guard, fired, timeout};
    }

private:
    // Free functions: parameters stored by value in the coroutine frame.
    static task<void>
    channel_timer(std::shared_ptr<channel_select_state> state, std::coroutine_handle<> h, qb::duration delay) {
        co_await sleep(delay);
        // winner=1 signals timeout; schedule_via_current deduplicates
        if (!state->resolved) {
            state->resolved = true;
            state->winner   = 1;
            schedule_via_current(h);
        }
    }

    static task<void>
    send_timer(std::shared_ptr<bool> guard, std::shared_ptr<bool> fired, std::coroutine_handle<> h, qb::duration delay) {
        co_await sleep(delay);
        if (!*guard) {
            *guard = true;
            *fired = true;
            schedule_via_current(h);
        }
    }

    size_t _capacity;
    using recv_waiter_entry = std::pair<std::coroutine_handle<>, std::optional<T> *>;

    struct send_waiter_entry {
        std::coroutine_handle<> handle;
        std::shared_ptr<bool>   guard; // set to true on wake (prevents timer double-schedule)
    };

    struct select_waiter_entry {
        std::shared_ptr<channel_select_state> state;
        size_t                                index;
    };

    void
    wake_one_sender() {
        while (!_send_waiters.empty()) {
            auto entry = _send_waiters.front();
            _send_waiters.pop_front();
            if (entry.guard && *entry.guard)
                continue; // already woken by timer
            if (entry.guard)
                *entry.guard = true;
            schedule_via_current(entry.handle);
            return;
        }
    }

    std::queue<T>                   _buffer;
    std::deque<send_waiter_entry>   _send_waiters;
    std::deque<recv_waiter_entry>   _recv_waiters;
    std::deque<select_waiter_entry> _select_waiters;
    bool                            _closed = false;
    // Liveness token, set false by ~channel. close() resumes parked waiters via the
    // scheduler (deferred), and if the channel owner frees the channel before that
    // resume runs — e.g. a coroutine suspended on `co_await consumer.receive()` while
    // the consumer is destroyed — the resumed await_resume() and the awaiter destructor
    // would touch freed channel state (_buffer / _recv_waiters / _send_waiters). Each
    // awaiter captures this token by value and no-ops its channel access when it is
    // false, so the deferred resume completes (recv → nullopt, send → channel_closed)
    // without a use-after-free.
    std::shared_ptr<bool> _alive{std::make_shared<bool>(true)};
};

/**
 * @brief Helper to create a channel
 * @tparam T Value type
 * @param capacity Channel buffer capacity
 * @return Unique pointer to new channel
 * @ingroup Coroutine
 */
template <typename T>
auto
make_channel(size_t capacity = 0) {
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
    channel<T> &_ch;

public:
    explicit channel_range(channel<T> &ch)
        : _ch(ch) {}

    class iterator {
        channel<T>      &_ch;
        std::optional<T> _current;
        bool             _done = false;

    public:
        iterator(channel<T> &ch, bool end = false)
            : _ch(ch)
            , _done(end) {
            if (!end) {
                advance();
            }
        }

        void
        advance() {
            // Non-blocking: see class doc. Use async_stream for true async
            // iteration.
            _current = _ch.try_recv();
            if (!_current) {
                _done = true;
            }
        }

        bool
        operator!=(const iterator &other) const {
            return _done != other._done;
        }

        iterator &
        operator++() {
            advance();
            return *this;
        }

        T
        operator*() {
            return std::move(*_current);
        }
    };

    iterator
    begin() {
        return iterator{_ch};
    }
    iterator
    end() {
        return iterator{_ch, true};
    }
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
task<void>
transform(channel<T> &in, channel<U> &out, F f) {
    while (true) {
        auto val = co_await in.recv();
        if (!val)
            break;

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
task<void>
filter(channel<T> &in, channel<T> &out, F pred) {
    while (true) {
        auto val = co_await in.recv();
        if (!val)
            break;

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
task<std::vector<T>>
collect(channel<T> &ch) {
    std::vector<T> result;

    while (true) {
        auto val = co_await ch.recv();
        if (!val)
            break;
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
 * Lifetime: the worker CO-OWNS both channels via shared_ptr captured by value in its coroutine
 * frame, so they stay alive for as long as the worker runs regardless of when the caller drops its
 * own handles. The caller therefore has no "keep the channels alive" obligation, and the worker can
 * never dereference (recv/send/close) a freed channel — the previous raw-pointer design left the
 * loop-exit `close()` (and an in-flight `send`) writing through freed memory if the caller dropped
 * the channels early (the recv side was already liveness-guarded; close()/send were not).
 */
template <typename T, typename U, typename F>
auto
make_pipeline(F f, size_t buffer_size = 10) {
    auto in  = std::make_shared<channel<T>>(buffer_size);
    auto out = std::make_shared<channel<U>>(buffer_size);

    // The worker takes shared_ptr copies (value parameters in the frame) — it co-owns the channels
    // and uses a static function (not a lambda) so nothing dangles after make_pipeline returns.
    coro_scheduler().spawn(pipeline_worker<T, U>(std::move(f), in, out));

    return std::make_pair(std::move(in), std::move(out));
}

// Free function: fn, in, out are VALUE parameters stored in the coroutine frame by the standard
// (the channels are co-owned for the worker's whole lifetime). No dangling/UAF possible.
template <typename T, typename U, typename F>
task<void>
pipeline_worker(F fn, std::shared_ptr<channel<T>> in, std::shared_ptr<channel<U>> out) {
    // Close `out` on EVERY exit so a consumer parked on out->recv() wakes instead
    // of hanging forever when the transform `fn` (or a send) throws. make_pipeline
    // exposes no error channel, so a transform exception is reported downstream as
    // end-of-stream rather than rethrown (surfacing it would require a
    // make_pipeline API change) — but the pipeline never deadlocks.
    try {
        while (true) {
            auto val = co_await in->recv();
            if (!val)
                break;
            co_await out->send(fn(*val));
        }
    } catch (const channel_closed &) {
        // Downstream closed `out` — terminal state, not an error.
    } catch (...) {
        // Transform / send failed: fall through to close() so the consumer isn't stranded.
    }
    out->close();
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
    T
    get() const {
        return std::any_cast<T>(value);
    }
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
    std::tuple<channel<Ts> *...> _channels;

    // Pass 1: prefer any channel that has buffered data.
    template <size_t I>
    bool
    try_data() {
        auto *ch = std::get<I>(_channels);
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
    bool
    try_closed() {
        auto *ch = std::get<I>(_channels);
        if (ch->is_closed() && ch->empty()) {
            _state->resolve(I, std::any{}, true);
            return true;
        }
        if constexpr (I + 1 < sizeof...(Ts))
            return try_closed<I + 1>();
        return false;
    }

    bool
    try_immediate() {
        return try_data<0>() || try_closed<0>();
    }

    template <size_t... Is>
    void
    register_all(std::index_sequence<Is...>) {
        (std::get<Is>(_channels)->register_select_waiter(_state, Is), ...);
    }

public:
    explicit channel_select_awaiter(channel<Ts> &...chs)
        : _state(std::make_shared<state_t>())
        , _channels(&chs...) {}

    // If this select is destroyed while still parked (cancelled before any channel
    // resolved it), mark the shared state resolved and drop the outer handle so a later
    // sender that finds the stale _select_waiters entry skips it (resolve() is a no-op
    // once resolved) instead of scheduling our now-destroyed coroutine handle — a
    // use-after-free. Only the refcounted _state is touched, never the channels (which
    // may already be freed).
    //
    // NOTE (intentional, documented edge): unlike recv()/recv_for() — which re-buffer a value handed
    // to a receiver that is then reclaimed — a multi-way select() that is RESOLVED with a value but
    // abandoned before resume (an outer when_any/race loser) DROPS that one value. Re-buffering it
    // would require a runtime-index→compile-time-type dispatch over the heterogeneous channel tuple;
    // and abandoning a whole multi-source select is semantically a choice-level abandon (you gave up
    // waiting on ALL of them), so dropping the single in-flight value is acceptable and consistent
    // across the variadic and vector select forms. Single-channel receives preserve the value; select
    // does not. If this ever matters, route the receive through recv()/recv_for() on one channel.
    ~channel_select_awaiter() {
        if (_state) {
            _state->resolved = true;
            _state->outer    = {};
        }
    }

    [[nodiscard]] bool
    await_ready() {
        return try_immediate();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        if (_state->resolved) {
            schedule_via_current(h);
            return;
        }
        _state->outer = h;
        register_all(std::make_index_sequence<sizeof...(Ts)>{});
    }

    select_result
    await_resume() {
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
auto
select(channel<Ts> &...chs) {
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
    std::vector<channel<T> *> _channels;

public:
    explicit channel_select_vector_awaiter(std::vector<channel<T> *> chs)
        : _state(std::make_shared<state_t>())
        , _channels(std::move(chs)) {}

    // See channel_select_awaiter::~channel_select_awaiter: prevent a stale select
    // waiter from scheduling a destroyed coroutine handle (use-after-free) when this
    // awaiter is cancelled while parked. Touches only the refcounted shared state.
    ~channel_select_vector_awaiter() {
        if (_state) {
            _state->resolved = true;
            _state->outer    = {};
        }
    }

    [[nodiscard]] bool
    await_ready() {
        // Pass 1: prefer data over close signals (avoids starvation).
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto *ch = _channels[i];
            if (!ch->empty()) {
                auto val = ch->try_recv();
                if (val) {
                    _state->resolve(i, std::any(std::move(*val)));
                    return true;
                }
            }
        }
        // Pass 2: report first closed-and-empty channel.
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto *ch = _channels[i];
            if (ch->is_closed() && ch->empty()) {
                _state->resolve(i, std::any{}, true);
                return true;
            }
        }
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        if (_state->resolved) {
            schedule_via_current(h);
            return;
        }
        _state->outer = h;
        for (size_t i = 0; i < _channels.size(); ++i)
            _channels[i]->register_select_waiter(_state, i);
    }

    select_result
    await_resume() {
        return {_state->winner, _state->closed, std::move(_state->value)};
    }
};

template <typename T>
auto
select(std::vector<channel<T> *> chs) {
    return channel_select_vector_awaiter<T>(std::move(chs));
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CHANNEL_H
