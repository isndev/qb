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
#include <qb/system/container/growable_ring.h>
#include <qb/system/time.h>          // qb::duration
#include <qb/utility/branch_hints.h> // QB_NOINLINE: the parked-sender hand-off stays out of the receivers' bodies
#include <any>                       // std::any / std::any_cast — the type-erased slot in every waiter
#include <array>                     // the select awaiter's liveness tokens, one per channel (Huly QB-273)
#include <cassert>                   // the select registration's caller-error checks (Huly QB-273)
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
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
 * and skip. The state is owned, by value, by the awaiter that registered it, and that
 * awaiter removes its entries on timeout and in its destructor (Huly QB-273). Between a
 * multi-channel select's resolution and its awaiter's destruction -- the rest of the turn, or
 * longer when the awaiter is named and outlives its `co_await` -- the channels that did not
 * win still hold its resolved entry: the hand-off paths' skip of a resolved entry is REQUIRED,
 * it is what keeps a send on such a channel from being reported delivered and dropped.
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
 * @brief The parties parked on a `channel<T>`, as `channel<T>::waiter_census()` reports them (Huly QB-273).
 * @ingroup Coroutine
 */
struct channel_waiter_census {
    std::size_t receivers{0}; ///< parked `recv()`
    std::size_t senders{0};   ///< parked `send()` and `send_for()`
    std::size_t selects{0};   ///< parked `select()` and `recv_for()` registrations
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
     * @brief A parked sender's node in the channel's list of senders (Huly QB-273): a `send()` or `send_for()`
     *        awaiter embeds one, so parking allocates nothing and leaving -- a hand-off, a timeout, a close, a
     *        reclaim -- unlinks it in O(1) wherever it sits. A node must not move while `linked`.
     */
    struct send_waiter_entry {
        std::coroutine_handle<> handle{};
        bool                   *guard{nullptr};     // send_for only: the awaiter's own flag, set by the wake, the watchdog, close()
        T                      *value{nullptr};     // the parked sender's value, handed over by wake_one_sender()
        bool                   *delivered{nullptr}; // set by wake_one_sender() once `value` is handed over
        send_waiter_entry      *prev{nullptr};
        send_waiter_entry      *next{nullptr};
        bool                    linked{false};
    };

    /**
     * @brief A `select()` / `recv_for()` registration's node in the channel's list of select waiters (Huly QB-273):
     *        non-owning, the state lives in the awaiter that registered it, and that awaiter unlinks the node on
     *        timeout and in its destructor while the channel lives -- so a node in the list is always a live
     *        waiter's, and a destroyed channel has no list left to hold one. A node must not move while `linked`.
     */
    struct select_waiter_entry {
        channel_select_state *state{nullptr};
        size_t                index{0};
        select_waiter_entry  *prev{nullptr};
        select_waiter_entry  *next{nullptr};
        channel const        *owner{nullptr}; ///< the channel it was linked into: the debug check of `unregister_select_waiter`
        bool                  linked{false};
    };
    using select_entry = select_waiter_entry; ///< the public name, for a caller of `register_select_waiter`

    /**
     * @brief Awaiter for send operation
     *
     * Fast path: await_ready() returns true when send can complete without
     * suspending (buffer has space or a receiver is waiting).
     */
    struct send_awaiter {
        static constexpr char const *qb_suspension_kind = "channel send"; ///< suspension tracking (coroutine/tracking.h)

        channel              &ch;
        T                     value;
        bool                  _completed = false;
        send_waiter_entry     _entry{};  ///< our node in ch's list of senders while parked (Huly QB-273)
        std::shared_ptr<bool> _ch_alive; ///< channel liveness; skip ch access when false

        // Explicit constructor: the deleted copy below (a user-declared constructor) makes this
        // a non-aggregate, so send() needs a constructor for brace-init.
        send_awaiter(channel &c, T v)
            : ch(c)
            , value(std::move(v))
            , _ch_alive(c._alive) {}

        // If this sender is still parked when its coroutine frame is destroyed,
        // unlink its node from the channel's list of senders so a later wake_one_sender()/close()
        // cannot schedule_via_current() a dangling handle (use-after-free).
        // Neither copyable nor movable (Huly QB-1000): it queues its own handle with pointers to its value and
        // completion flag, and its destructor retracts that entry, so a copy destroyed first retracted the live send
        // -- and a "move" was one, the user-declared destructor suppressing the implicit move. send() returns a
        // prvalue.
        send_awaiter(const send_awaiter &)            = delete;
        send_awaiter(send_awaiter &&)                 = delete;
        send_awaiter &operator=(const send_awaiter &) = delete;
        send_awaiter &operator=(send_awaiter &&)      = delete;

        ~send_awaiter() {
            if (_entry.linked && *_ch_alive) // still parked on a live channel: retract (a freed channel has no list)
                ch.unlink_sender(_entry);
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
            // A waiting receiver, then a waiting select waiter, then the buffer -- deliver() is the one hand-off.
            _completed = ch.deliver(std::move(value));
            return _completed;
        }

        // Only parks: await_ready() has just found nobody to take the value, and nothing can run in between. The
        // entry carries the value itself: the wake that finds room for it hands it over (wake_one_sender()).
        void
        await_suspend(std::coroutine_handle<> h) {
            ::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);
            _entry.handle    = h;
            _entry.value     = &value;
            _entry.delivered = &_completed;
            ch.link_sender(_entry);
        }

        void
        await_resume() {
            // Delivered -- at once, or by the wake that resumed us (the value moved into the channel or straight to a
            // receiver then, Huly QB-272). Nothing of the channel is touched here: it may already be gone.
            if (_completed)
                return;
            // Resumed without a hand-off: close() woke us, or the channel was destroyed (whose destructor closes it).
            // Finding 2.C.1: the value must be rejected loudly rather than silently dropped -- the
            // "@throws channel_closed" of send(), the counterpart of try_send's false.
            throw channel_closed();
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
        static constexpr char const *qb_suspension_kind = "channel recv"; ///< suspension tracking (coroutine/tracking.h)

        channel              &ch;
        std::optional<T>      _result;
        bool                  _resumed = false; ///< set in await_resume: distinguishes woken-then-reclaimed
        std::shared_ptr<bool> _ch_alive;        ///< channel liveness; skip ch access when false

        // Explicit constructor: the deleted copy below (a user-declared constructor) makes this a
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
        // in _recv_waiters AND never resumed AND _result holds a value) and give it back so the next
        // receiver gets it. Gated on !_resumed: on the normal path await_resume std::move's _result
        // (which leaves the optional engaged-but-moved-from), so without the gate we would give back a
        // moved-from value (double/garbage delivery).
        // Neither copyable nor movable (Huly QB-1000): see send_awaiter; this one queues the address of its own
        // result slot, and its destructor retracts it or puts back a value handed but never read.
        recv_awaiter(const recv_awaiter &)            = delete;
        recv_awaiter(recv_awaiter &&)                 = delete;
        recv_awaiter &operator=(const recv_awaiter &) = delete;
        recv_awaiter &operator=(recv_awaiter &&)      = delete;

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
            // consuming it → return it to the channel so the message is not lost: through the one hand-off first
            // -- a receiver or a select waiter parked since takes it, as it would have taken a send -- and to the
            // buffer only when nobody can (Huly QB-273: straight to the buffer, it skipped a parked receiver, which
            // stayed parked, and put a value in a capacity-0 channel's buffer).
            if (!_resumed && _result.has_value())
                ch.restore(std::move(*_result));
        }

        [[nodiscard]] bool
        await_ready() {
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop_front();
                ch.wake_one_sender();
                return true;
            }
            if (ch._closed)
                return true;
            return false;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            ::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);
            if (!ch._buffer.empty()) {
                _result = std::move(ch._buffer.front());
                ch._buffer.pop_front();
                ch.wake_one_sender();
                schedule_via_current(h);
            } else if (ch._closed) {
                schedule_via_current(h);
            } else {
                ch._recv_waiters.push_back({h, &_result});
                // Rendezvous (capacity==0) sender-first: if a sender is already parked,
                // wake one -- the wake hands its value straight to this newly parked receiver.
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
                ch._buffer.pop_front();
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
        return deliver(value);
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
        return deliver(std::move(value));
    }

    /**
     * @brief Non-blocking try receive
     * @return Optional value - empty if channel empty or closed with no data
     */
    std::optional<T>
    try_recv() {
        if (!_buffer.empty()) {
            T value = std::move(_buffer.front());
            _buffer.pop_front();
            wake_one_sender_if_any();
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
        while (auto *entry = _send_head) {
            unlink_sender(*entry); // its awaiter's destructor finds nothing to retract
            if (entry->guard && *entry->guard)
                continue;
            if (entry->guard)
                *entry->guard = true;
            schedule_via_current(entry->handle);
        }
        // Notify select waiters with "closed" signal
        while (auto *entry = _select_head) {
            unlink_select(*entry);
            entry->state->resolve(entry->index, std::any{}, /*closed=*/true);
        }
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

    /**
     * @brief The parties parked on this channel right now (Huly QB-273): the size of its ring of receivers and
     *        the counters of its two lists, senders and select registrations.
     * @details `receivers` are the parked `recv()`, `senders` the parked `send()` and `send_for()`, `selects`
     *          the `select()` and `recv_for()` registrations. An entry never outlives its waiter -- a timed wait
     *          that expired, a `select()` another channel won, a frame reclaimed while parked leave nothing
     *          here -- so these are the live population, not a history: the census a resource test reads. One
     *          transient is counted: from a multi-channel `select()`'s resolution until its awaiter is destroyed,
     *          the channels that did not win still hold its resolved entry -- the rest of that turn for a
     *          `co_await select(...)`, as long as the awaiter lives when it is named (`auto s = select(a, b);
     *          co_await s;`). Three reads: the cost of `size()`.
     */
    [[nodiscard]] channel_waiter_census
    waiter_census() const noexcept {
        return {_recv_waiters.size(), _send_count, _select_count};
    }

    /**
     * @brief This channel's liveness token: `true` while the channel exists, `false` once its destructor ran
     *        (Huly QB-273). A `select()` awaiter copies it BEFORE registering on the channel, so that whatever
     *        ends its wait -- a resume, a reclaim, an exception thrown by the registration's own wake -- it can
     *        tell a live channel it may retract from, from a dead one it must not touch. A copy, read-only: only
     *        the channel's destructor clears it.
     */
    [[nodiscard]] std::shared_ptr<bool const>
    liveness() const noexcept {
        return _alive;
    }

    // -----------------------------------------------------------------------
    // select() support
    // -----------------------------------------------------------------------

    /**
     * @brief Register a select operation as a potential receiver on this channel.
     *
     * Called by channel_select_awaiter before suspending. When a value becomes
     * available (via send or close), this channel will call state.resolve() if
     * the select is not yet resolved.
     *
     * The node and the state are the caller's (Huly QB-273): `entry` is linked into this channel's list and
     * points at `state`, so neither may move or die while the node is linked. The caller's side of the
     * contract: copy `liveness()` BEFORE calling this (the wake below may throw, and the caller must still
     * know whether it may retract), call `unregister_select_waiter(entry)` before the state goes away while
     * this channel is alive -- on its timeout and in its destructor -- and never touch this channel once the
     * token reads false (the channel's destructor closed it, which resolved the state and unlinked the node).
     * A state already resolved (an earlier channel of the same select delivered during this registration) is
     * not queued. A node is registered on one channel at a time: registering one still linked is a caller error,
     * asserted in a debug build (it would rewrite the list it hangs in).
     */
    void
    register_select_waiter(select_waiter_entry &entry, channel_select_state &state, size_t idx) {
        assert(!entry.linked && "channel::register_select_waiter: the node is already linked");
        if (state.resolved)
            return;
        // Fast path: value already in buffer
        if (!_buffer.empty()) {
            T val = std::move(_buffer.front());
            _buffer.pop_front();
            wake_one_sender_if_any();
            state.resolve(idx, std::any(std::move(val)));
            return;
        }
        if (_closed) {
            state.resolve(idx, std::any{}, true);
            return;
        }
        entry.state = &state;
        entry.index = idx;
        link_select(entry);
        // Rendezvous (capacity==0) sender-first: if a sender is already parked with a value, wake one
        // -- the wake hands its value to this select/recv_for waiter. Without this, select() and
        // recv_for() never observe a pending rendezvous value that recv()/try_recv() would (a parked
        // sender is only ever served by a wake). Mirrors recv_awaiter::await_suspend.
        wake_one_sender();
    }

    /**
     * @brief Unlink the caller's node, if this channel still holds it. O(1), idempotent.
     * @details The waiter's side of the contract above: called on its timeout and from its destructor, so the
     *          list never holds the node of a waiter that is gone -- the bound of Huly QB-273. Nothing happens
     *          when the node was already handed a value (unlinked by the delivery) or dropped by `close()`. The
     *          node must be this channel's: one linked into another channel is a caller error, asserted in a debug
     *          build (it would rewrite this channel's list and counter).
     */
    void
    unregister_select_waiter(select_waiter_entry &entry) noexcept {
        assert((!entry.linked || entry.owner == this) && "channel::unregister_select_waiter: the node is linked into another channel");
        if (entry.linked)
            unlink_select(entry);
    }

    /**
     * @brief True iff a `select()` / `recv_for()` waiter is currently parked and
     *        unresolved — i.e. `send()`'s fast path can hand it a value right now.
     * @details `send_for()` must consult this (not just `_recv_waiters` / buffer
     *          space): a `recv_for()` parks in the select waiters' list, not
     *          `_recv_waiters`, so a `send_for()` that ignored it would take the
     *          slow path and park with a timer while a ready rendezvous partner
     *          waits — both then time out. The skip of a resolved entry is REQUIRED (see
     *          `channel_select_state`): a multi-channel select resolved elsewhere keeps its entry
     *          here until its awaiter is destroyed. Since Huly QB-273 nothing else stays, so this
     *          scans the parked waiters, not a history. True on a move-only channel too, where no
     *          send can serve the waiter: `send_for()` asks it only for a copyable `T`.
     */
    [[nodiscard]] bool
    has_live_select_waiter() const noexcept {
        for (auto const *entry = _select_head; entry; entry = entry->next)
            if (!entry->state->resolved)
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
     *
     * @warning **Move-only `T` (e.g. `std::unique_ptr`): this wakes only on the buffered fast
     *          path.** The slow path parks as a `select()` registration, whose state -- the
     *          awaiter's own -- type-erases the delivered value through `std::any`, and `std::any`
     *          requires a CopyConstructible type. A sender therefore cannot hand a move-only value
     *          to a parked `recv_for`, so the wait always runs to its full `timeout` and reports
     *          empty even if a value arrives meanwhile.
     *
     *          **Nothing is lost**: the sender buffers the value (or stays parked on a rendezvous
     *          channel), so the next `recv()` / `recv_for()` picks it up. The cost is latency, not
     *          data. For a move-only payload that needs prompt timed delivery, prefer plain
     *          `recv()` plus an external deadline, or box the payload in a copyable handle
     *          (`std::shared_ptr`).
     *
     *          This is a constraint of the `std::any`-based `select()` machinery — `select_result`
     *          exposes `get<T>()` by value as public API — not of this function alone.
     *
     * @note A parked wait holds a watchdog `ev_timer` on its thread's loop, as `sleep()` does, and is
     *       registered with the thread's scheduler while it is armed: the listener's teardown destroys
     *       it with the other parked frames, and a frame kept beyond that listener (a task held by a
     *       static, say) must not be destroyed after it -- `timer_awaiter`'s contract.
     */
    task<std::optional<T>>
    recv_for(qb::duration timeout) {
        // Fast path
        if (!_buffer.empty()) {
            co_return try_recv();
        }
        if (_closed)
            co_return std::nullopt;

        // index 0 = data received; winner 1 = the watchdog fired. The awaiter owns its select state and its
        // watchdog, both inside the recv_for frame (Huly QB-273): no shared state, no spawned timer coroutine, no
        // allocation -- and nothing of this wait outlives it: the watchdog is stopped when the wait ends, the
        // node unlinked from the channel's select list by the timeout or the destructor while the channel lives.
        struct timed_recv_awaiter {
            channel<T>             &ch;
            channel_select_state    state{};
            select_waiter_entry     entry{}; ///< our node in ch's list of select waiters while parked
            qb::duration            timeout_ms;
            ev_timer                watchdog{};
            ev::loop_ref            loop;                 ///< the loop the watchdog is armed on: `listener::current`'s (Huly QB-197)
            CoroutineScheduler     *_scheduler = nullptr; ///< where the frame is registered while armed (timer_awaiter's rule)
            std::coroutine_handle<> _handle{};            ///< the frame registered there
            bool                    _armed   = false;     ///< the watchdog was started: stop it unconditionally (timer_awaiter's rule)
            bool                    _resumed = false;     ///< distinguishes resolved-then-reclaimed
            std::shared_ptr<bool>   _ch_alive;            ///< channel liveness; skip ch access when false

            timed_recv_awaiter(channel<T> &c, qb::duration t)
                : ch(c)
                , timeout_ms(t)
                , loop(listener::current.loop())
                , _ch_alive(c._alive) {
                ev_timer_init(&watchdog, on_watchdog, qb::detail::to_ev_seconds(t), 0.0);
                watchdog.data = this;
            }

            // Frame-destruction guard: this awaiter lives inside the recv_for coroutine frame. If that frame is
            // destroyed while parked, the watchdog is stopped and the entry retracted, so neither can reach the
            // now-dangling handle, and the state is closed so a resume already queued finds nothing to do.
            //
            // Wake-token preservation (mirrors recv_awaiter): if a sender already RESOLVED us with a
            // value (state.resolved, not a timeout/close) but we are reclaimed before consuming it,
            // hand that value back to the channel (restore(): a waiter parked since, else the buffer) so the
            // sender's message is not lost. Guarded by _ch_alive (the channel may be torn down first) and
            // !_resumed (await_resume already took the value).
            // Neither copyable nor movable (Huly QB-1000): see send_awaiter; its destructor retracts the entry the
            // live wait is parked on, stops its watchdog, or restores a value it was handed and never read.
            timed_recv_awaiter(const timed_recv_awaiter &)            = delete;
            timed_recv_awaiter(timed_recv_awaiter &&)                 = delete;
            timed_recv_awaiter &operator=(const timed_recv_awaiter &) = delete;
            timed_recv_awaiter &operator=(timed_recv_awaiter &&)      = delete;

            ~timed_recv_awaiter() {
                // timer_awaiter's unschedule(): a frame destroyed by destroy_all_suspended() -- not through its task
                // -- after a delivery queued it must leave the ready queue and the suspended set with it.
                if (_scheduler)
                    _scheduler->forget(_handle);
                disarm();
                if (!state.resolved) {
                    unregister();
                    state.resolved = true;
                    state.outer    = {};
                    return;
                }
                // `if constexpr`: `std::any_cast<T>` carries a hard `static_assert` that T be
                // CopyConstructible, and libstdc++ fires it at INSTANTIATION — so leaving this
                // unguarded made `channel<std::unique_ptr<...>>::recv_for()` fail to COMPILE on
                // Linux/libstdc++ even though the branch can never run there. (libc++ accepts it,
                // which is why macOS never saw this.) For a move-only T nothing can populate
                // `state.value` in the first place — see the select-machinery note on
                // `offer_to_select_waiter` — so the branch is provably dead, not merely untaken.
                if constexpr (std::is_copy_constructible_v<T>) {
                    if (!_resumed && *_ch_alive && state.winner != 1 && !state.closed && state.value.has_value())
                        ch.restore(std::any_cast<T>(std::move(state.value)));
                }
            }

            /// Stop the watchdog whenever it was armed -- never gated on `ev_is_active`: an expired one-shot is
            /// inactive yet still pending with `data` pointing here (the rule written out in ~timer_awaiter).
            void
            disarm() noexcept {
                if (_armed) {
                    ev_timer_stop(loop, &watchdog);
                    _armed = false;
                }
            }

            /// Leave the scheduler's suspended set, as timer_awaiter does at its resume and its watcher's callback.
            void
            unregister_suspended() noexcept {
                if (_scheduler)
                    _scheduler->unregister_suspended(_handle);
            }

            /// Unlink our node from the channel's list, if it still hangs there and the channel lives.
            void
            unregister() noexcept {
                if (entry.linked && *_ch_alive)
                    ch.unregister_select_waiter(entry);
            }

            [[nodiscard]] bool
            await_ready() const noexcept {
                return state.resolved;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                ::qb::io::async::detail::track_suspension(h.address(), "channel recv");
                if (state.resolved) {
                    schedule_via_current(h);
                    return;
                }
                state.outer = h;
                entry.state = &state;
                entry.index = 0;
                ch.link_select(entry);
                // Rendezvous (capacity==0) sender-first: wake a parked sender -- the wake hands its value
                // to this recv_for waiter. Mirrors register_select_waiter / recv_awaiter::await_suspend.
                ch.wake_one_sender();
                if (state.resolved)
                    return; // the wake handed a value over (and scheduled us): no watchdog to arm
                if (timeout_ms.count() <= 0) {
                    // A zero or negative timeout is one cooperative turn, not a kernel timer (timer_awaiter's
                    // rule): back of the ready queue; still unresolved when we resume = timed out.
                    enqueue_for_later_via_current(h);
                    return;
                }
                // Parked on a watcher: in the scheduler's suspended set while armed, as timer_awaiter is -- so
                // active_count(), dump(), the abandoned-frame report and destroy_all_suspended() see the wait.
                _handle    = h;
                _scheduler = CoroutineScheduler::current_ptr();
                if (!_scheduler)
                    _scheduler = &CoroutineScheduler::current();
                _scheduler->register_suspended(h);
                // A stale loop clock would compute the expiry in the past and fire at once: refresh it first,
                // the fix timer_awaiter and async::callback carry (Finding 2.C.3).
                ev_now_update(static_cast<struct ev_loop *>(loop));
                ev_timer_start(loop, &watchdog);
                _armed = true;
            }

            std::optional<T>
            await_resume() {
                _resumed = true; // consumed normally → the dtor must not re-buffer the value
                disarm();
                unregister_suspended();
                if (!state.resolved) {
                    // The cooperative turn of a zero timeout ended with nobody delivering: a timeout.
                    state.resolved = true;
                    state.winner   = 1;
                    unregister();
                    return std::nullopt;
                }
                if (state.winner == 1 || state.closed)
                    return std::nullopt;
                // Same `if constexpr` rationale as the destructor above: `std::any_cast<T>` must
                // not be INSTANTIATED for a move-only T (hard static_assert under libstdc++).
                if constexpr (std::is_copy_constructible_v<T>) {
                    if (!state.value.has_value())
                        return std::nullopt;
                    return std::any_cast<T>(std::move(state.value));
                } else {
                    // Move-only payload: the select machinery cannot carry it (documented on
                    // `recv_for` above), so the slow path only ever resolves as a timeout.
                    return std::nullopt;
                }
            }

            /// The watchdog: the deadline passed with the wait still open. One resume, deduplicated by the
            /// scheduler against a same-turn delivery; the entry leaves the list here, not at a later send.
            static void
            on_watchdog(struct ev_loop *, ev_timer *w, int) noexcept {
                auto *self = static_cast<timed_recv_awaiter *>(w->data);
                if (!self || self->state.resolved)
                    return; // a value or close() got there first; the resume stops this watcher
                self->state.resolved = true;
                self->state.winner   = 1;
                self->unregister();
                self->unregister_suspended(); // out of the suspended set before the resume is queued (on_event_ready)
                schedule_via_current(std::exchange(self->state.outer, {}));
            }
        };

        // A local awaiter no test can name: the oracle of Huly QB-1000 sits beside its definition.
        static_assert(!std::is_copy_constructible_v<timed_recv_awaiter> && !std::is_move_constructible_v<timed_recv_awaiter>);
        static_assert(!std::is_copy_assignable_v<timed_recv_awaiter> && !std::is_move_assignable_v<timed_recv_awaiter>);

        co_return co_await timed_recv_awaiter{*this, timeout};
    }

    /**
     * @brief Send with timeout
     * @param value  Value to send
     * @param timeout Maximum time to wait for buffer space
     * @return true if sent, false on timeout or closed channel
     *
     * A move-only `T` is handed over by every path a parked receiver or a free slot offers, and never to a
     * `select()` or `recv_for()` waiter (see `recv_for`): with only such a waiter parked, the value waits for a
     * receiver or a slot until its deadline, as it would on an empty channel. The watchdog note of `recv_for`
     * holds here too.
     */
    task<bool>
    send_for(T value, qb::duration timeout) {
        if (_closed)
            co_return false;
        // Fast path: a receiver, a live select()/recv_for() waiter, or buffer
        // space can take the value synchronously. send()'s await_ready performs
        // the actual recv/select/buffer hand-off. The has_live_select_waiter()
        // term is the fix for send_for parking with a timer while a ready
        // recv_for/select partner waits — on a rendezvous channel both would
        // otherwise time out despite a match. It counts only for a copyable T: no send hands a move-only value to
        // a select waiter (offer_to_select_waiter), so for one the fast path's send() parked with no watchdog and
        // ignored the timeout (Huly QB-273).
        if (_buffer.size() < _capacity || !_recv_waiters.empty() || (std::is_copy_constructible_v<T> && has_live_select_waiter())) {
            co_await send(std::move(value));
            co_return true;
        }

        // Slow path: wait for space or timeout. The awaiter owns its guard and its watchdog, inside the send_for
        // frame (Huly QB-273): the guard is the flag whichever ends the wait first sets -- the hand-off
        // (wake_one_sender), the watchdog, close(), the destructor -- so the others do nothing; the watchdog is
        // stopped when the wait ends and the node unlinked from the channel's sender list by the timeout or the destructor
        // while the channel lives. No shared flag, no spawned timer coroutine, no allocation.
        struct timed_send_awaiter {
            channel<T>             &ch;
            T                       val;
            qb::duration            timeout_ms;
            ev_timer                watchdog{};
            ev::loop_ref            loop;                 ///< the loop the watchdog is armed on: `listener::current`'s (Huly QB-197)
            CoroutineScheduler     *_scheduler = nullptr; ///< where the frame is registered while armed (timer_awaiter's rule)
            std::coroutine_handle<> _handle{};
            send_waiter_entry       _entry{};           ///< our node in ch's list of senders while parked
            bool                    _guard     = false; ///< set by whichever ends the wait; `send_waiter_entry::guard` points here
            bool                    _delivered = false; ///< set by wake_one_sender() when it hands `val` over
            bool                    _armed     = false; ///< the watchdog was started: stop it unconditionally (timer_awaiter's rule)
            std::shared_ptr<bool>   _ch_alive;          ///< channel liveness; skip ch access when false

            timed_send_awaiter(channel<T> &c, T v, qb::duration t)
                : ch(c)
                , val(std::move(v))
                , timeout_ms(t)
                , loop(listener::current.loop())
                , _ch_alive(c._alive) {
                ev_timer_init(&watchdog, on_watchdog, qb::detail::to_ev_seconds(t), 0.0);
                watchdog.data = this;
            }

            // Frame-destruction guard: if the send_for frame is destroyed while parked, the guard is set, the
            // watchdog stopped and the entry retracted, so nothing can hand `val` over or resume the dangling
            // handle. Neither copyable nor movable (Huly QB-1000): see send_awaiter; its destructor sets the guard
            // and retracts the queued value, out from under the live send.
            timed_send_awaiter(const timed_send_awaiter &)            = delete;
            timed_send_awaiter(timed_send_awaiter &&)                 = delete;
            timed_send_awaiter &operator=(const timed_send_awaiter &) = delete;
            timed_send_awaiter &operator=(timed_send_awaiter &&)      = delete;

            ~timed_send_awaiter() {
                if (_scheduler)
                    _scheduler->forget(_handle); // timer_awaiter's unschedule(): see ~timed_recv_awaiter
                disarm();
                if (!_guard) {
                    _guard = true;
                    unregister();
                }
            }

            /// Stop the watchdog whenever it was armed (never gated on `ev_is_active`: see ~timer_awaiter).
            void
            disarm() noexcept {
                if (_armed) {
                    ev_timer_stop(loop, &watchdog);
                    _armed = false;
                }
            }

            /// Leave the scheduler's suspended set (see timed_recv_awaiter).
            void
            unregister_suspended() noexcept {
                if (_scheduler)
                    _scheduler->unregister_suspended(_handle);
            }

            /// Unlink our node from the channel's list, if it still hangs there and the channel lives.
            void
            unregister() noexcept {
                if (_entry.linked && *_ch_alive)
                    ch.unlink_sender(_entry);
            }

            [[nodiscard]] bool
            await_ready() const noexcept {
                return false;
            }

            void
            await_suspend(std::coroutine_handle<> h) {
                ::qb::io::async::detail::track_suspension(h.address(), "channel send");
                _handle          = h;
                _entry.handle    = h;
                _entry.guard     = &_guard;
                _entry.value     = &val;
                _entry.delivered = &_delivered;
                ch.link_sender(_entry);
                if (timeout_ms.count() <= 0) {
                    // One cooperative turn, not a kernel timer (timer_awaiter's rule): a wake in that turn still
                    // hands `val` over; still parked when we resume = timed out.
                    enqueue_for_later_via_current(h);
                    return;
                }
                _scheduler = CoroutineScheduler::current_ptr(); // registered while armed: see timed_recv_awaiter
                if (!_scheduler)
                    _scheduler = &CoroutineScheduler::current();
                _scheduler->register_suspended(h);
                ev_now_update(static_cast<struct ev_loop *>(loop)); // see timed_recv_awaiter: never arm on a stale loop clock
                ev_timer_start(loop, &watchdog);
                _armed = true;
            }

            // True when the wake that resumed us handed the value over (Huly QB-272: the hand-off happens at the wake,
            // with the same priority as a direct send -- a waiting receiver, then a select/recv_for waiter, then the
            // buffer). Resumed without it: the watchdog fired, close() woke us, or the channel was destroyed -- false,
            // and nothing of the channel is touched (it may already be gone).
            bool
            await_resume() noexcept {
                disarm();
                unregister_suspended();
                if (!_guard) {
                    // The cooperative turn of a zero timeout ended without a hand-off: a timeout.
                    _guard = true;
                    unregister();
                }
                return _delivered;
            }

            /// The watchdog: the deadline passed with the value still queued. One resume, deduplicated by the
            /// scheduler against a same-turn wake; the entry leaves the list here, not at a later wake.
            static void
            on_watchdog(struct ev_loop *, ev_timer *w, int) noexcept {
                auto *self = static_cast<timed_send_awaiter *>(w->data);
                if (!self || self->_guard)
                    return; // a wake or close() got there first; the resume stops this watcher
                self->_guard = true;
                self->unregister();
                self->unregister_suspended(); // out of the suspended set before the resume is queued (on_event_ready)
                schedule_via_current(self->_handle);
            }
        };

        // A local awaiter no test can name: the oracle of Huly QB-1000 sits beside its definition.
        static_assert(!std::is_copy_constructible_v<timed_send_awaiter> && !std::is_move_constructible_v<timed_send_awaiter>);
        static_assert(!std::is_copy_assignable_v<timed_send_awaiter> && !std::is_move_assignable_v<timed_send_awaiter>);

        co_return co_await timed_send_awaiter{*this, std::move(value), timeout};
    }

private:
    /**
     * @brief Hand `value` to the first unresolved parked `select()` / `recv_for()` waiter, if there is one.
     * @return `true` when the value was delivered (resolve() schedules that waiter's outer
     *         coroutine); `false` when no live select waiter is queued — the caller then
     *         falls through to its own next step (buffer, or park).
     *
     * THE single select hand-off. Every send path must go through it, because `select()`
     * type-erases the delivered value through `std::any`, and `std::any` requires a
     * COPY-CONSTRUCTIBLE payload. A move-only `channel<T>` (e.g. `channel<std::unique_ptr<X>>`)
     * can hold a select waiter -- a `recv_for()` registers one for any `T` -- but no send can serve
     * it: the block is compiled out there, and `send_for()` does not count such a waiter as a taker.
     * Without the `if constexpr` it would still have to INSTANTIATE, turning every send into a hard
     * compile error. That guard used to exist on `try_send(T&&)` alone, so `try_send` worked on
     * a move-only channel while `co_await send(...)` / `send_for(...)` did not compile. Keeping
     * exactly one guarded copy is what stops that from drifting apart again.
     *
     * The skip of a resolved node is REQUIRED (see `channel_select_state`): a multi-channel select that
     * another channel resolved keeps its node here until its awaiter is destroyed, and without the test the
     * value would be "delivered" to that state -- resolve() is a no-op once resolved -- and dropped while the
     * send reported success. The value goes into its `std::any` BEFORE the node is unlinked: a copy or a move
     * that throws leaves the waiter parked and linked, and the exception to the sender. resolve() only queues
     * the resume, so the unlink after it still precedes everything the waiter runs.
     */
    template <typename U>
    bool
    offer_to_select_waiter(U &&value) {
        if constexpr (std::is_copy_constructible_v<T>) {
            // The common path -- nobody selecting -- is one load and one branch, tested BEFORE the loop: inlined,
            // the loop's register promotion of the three list members otherwise puts its pre-header and its
            // write-back epilogue on every try_send (+2.5 instructions per op measured on g++ 14, Huly QB-273).
            if (!_select_head) [[likely]]
                return false;
            while (auto *entry = _select_head) {
                if (entry->state->resolved) {
                    unlink_select(*entry); // a select resolved by another channel: required skip, see above
                    continue;
                }
                entry->state->resolve(entry->index, std::any(std::forward<U>(value))); // built first: may throw
                unlink_select(*entry);
                return true;
            }
        } else {
            (void) value;
        }
        return false;
    }

    // The two intrusive lists (Huly QB-273): FIFO, the nodes owned by the parked awaiters, every operation O(1).
    void
    link_sender(send_waiter_entry &e) noexcept {
        e.prev                                       = _send_tail;
        e.next                                       = nullptr;
        (_send_tail ? _send_tail->next : _send_head) = &e;
        _send_tail                                   = &e;
        e.linked                                     = true;
        ++_send_count;
    }
    void
    unlink_sender(send_waiter_entry &e) noexcept {
        (e.prev ? e.prev->next : _send_head) = e.next;
        (e.next ? e.next->prev : _send_tail) = e.prev;
        e.prev = e.next = nullptr;
        e.linked        = false;
        --_send_count;
    }
    void
    link_select(select_waiter_entry &e) noexcept {
        e.prev                                             = _select_tail;
        e.next                                             = nullptr;
        (_select_tail ? _select_tail->next : _select_head) = &e;
        _select_tail                                       = &e;
        e.owner                                            = this;
        e.linked                                           = true;
        ++_select_count;
    }
    void
    unlink_select(select_waiter_entry &e) noexcept {
        (e.prev ? e.prev->next : _select_head) = e.next;
        (e.next ? e.next->prev : _select_tail) = e.prev;
        e.prev = e.next = nullptr;
        e.linked        = false;
        --_select_count;
    }

    size_t _capacity;
    using recv_waiter_entry = std::pair<std::coroutine_handle<>, std::optional<T> *>;

    /**
     * @brief Hand `value` to whoever can take it NOW: the first waiting receiver, else the first live `select()` /
     *        `recv_for()` waiter, else a free buffer slot.
     * @return `false` when nobody can (no waiter, buffer full or rendezvous): `value` is then left untouched.
     *
     * THE single send-side hand-off: `send()`'s fast path, both `try_send` overloads and the wake of a parked sender
     * all go through it, so the priority cannot drift between them -- it did, before: `try_send` once skipped the
     * select waiters and returned false on a rendezvous a parked `select()` was ready for.
     */
    template <typename U>
    bool
    deliver(U &&value) {
        if (!_recv_waiters.empty()) {
            auto [recv_h, result_ptr] = _recv_waiters.front();
            _recv_waiters.pop_front();
            *result_ptr = std::forward<U>(value);
            schedule_via_current(recv_h);
            return true;
        }
        if (offer_to_select_waiter(std::forward<U>(value)))
            return true;
        if (_buffer.size() < _capacity) {
            _buffer.push_back(std::forward<U>(value));
            return true;
        }
        return false;
    }

    /**
     * @brief Give back a value a receiver was handed and never read (its frame reclaimed first): through
     *        `deliver()` -- a receiver or a select waiter parked since takes it, as it would a send -- and into
     *        the buffer only when nobody can, past the capacity if the buffer is full or the channel has none:
     *        the value was sent, and it must not be lost (Huly QB-273).
     */
    void
    restore(T &&value) {
        if (!deliver(std::move(value)))
            _buffer.push_back(std::move(value));
    }

    /**
     * @brief Called wherever room for a parked sender may have appeared (a slot freed, a receiver or a select waiter
     *        parked): the first parked sender's value is handed over HERE, then the sender is resumed, already done.
     *
     * The wake is the delivery (Huly QB-272). Waking the sender to let it deliver on resume left a window: anything
     * that ran before that resume -- a `try_send`, another sender's fast path -- saw the room as free and took it;
     * the woken sender then buffered past the capacity, behind the newcomer, or on a capacity-0 channel. Handing the
     * value over before anything else runs keeps the capacity and the senders' FIFO order, and leaves nothing to give
     * back: a sender whose frame is reclaimed before it resumes (a `when_any` loser) has sent -- as a `send_for` woken
     * before its timer always had. The receive side mirrors it: a receiver handed a value that never resumes
     * gives it back (~recv_awaiter, restore()). Either way a value is delivered exactly once.
     *
     * Out of line, empty check included: inlined, this hand-off (two `pop_front` sites, an entry held across the
     * `deliver()` call) entered the receive loop of every caller, and g++-14 then kept the caller's own loop values
     * in memory -- `try_recv` +45 % (Huly QB-272). One call per pop on MSVC, where an inline empty check with only
     * the hand-off out of line measured +7 %; GCC and Clang take that inline check (`wake_one_sender_if_any`): it
     * was faster on g++ in QB-272's measurement, and on Apple clang 21 the call on every `try_recv` pop cost the
     * try_send/try_recv cell +2.3 % against v3.2.1 (macOS validation, 2026-10-24).
     */
    QB_NOINLINE void
    wake_one_sender() {
        while (auto *entry = _send_head) {
            if (entry->guard && *entry->guard) {
                unlink_sender(*entry); // guard already set: a safety net since Huly QB-273 (a timed-out or reclaimed send_for unlinks itself)
                continue;
            }
            if (!deliver(std::move(*entry->value)))
                return; // no room after all: it stays parked, first in line
            *entry->delivered = true;
            if (entry->guard)
                *entry->guard = true; // its watchdog must not resume it a second time
            const auto h = entry->handle;
            unlink_sender(*entry);
            schedule_via_current(h);
            return;
        }
    }

    /// The pop sites' wake: see `wake_one_sender` for why the test is inline on GCC and Clang and not on MSVC.
    void
    wake_one_sender_if_any() {
#if defined(_MSC_VER) && !defined(__clang__)
        wake_one_sender();
#else
        if (_send_head) [[unlikely]]
            wake_one_sender();
#endif
    }

    qb::growable_ring<T>                 _buffer;
    qb::growable_ring<recv_waiter_entry> _recv_waiters;
    send_waiter_entry                   *_send_head    = nullptr; ///< parked senders, FIFO (Huly QB-273: an intrusive list)
    send_waiter_entry                   *_send_tail    = nullptr;
    select_waiter_entry                 *_select_head  = nullptr; ///< select() / recv_for() registrations, FIFO
    select_waiter_entry                 *_select_tail  = nullptr;
    std::size_t                          _send_count   = 0;
    std::size_t                          _select_count = 0;
    bool                                 _closed       = false;
    // Liveness token, set false by ~channel. close() resumes parked waiters via the
    // scheduler (deferred), and if the channel owner frees the channel before that
    // resume runs — e.g. a coroutine suspended on `co_await consumer.receive()` while
    // the consumer is destroyed — the resumed await_resume() and the awaiter destructor
    // would touch freed channel state (_buffer, the receivers' ring, the senders' list). Each
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
 * Lifetime: a worker coroutine transforms every value `in` receives into `out` until `in` is closed, then closes
 * `out`. It co-owns both channels, so it never touches a freed one. The two handles returned, and every copy of them,
 * share one lease on it (Huly QB-981): when the last of them goes, the worker is destroyed wherever it waits, `in`
 * closed or not -- it transforms nothing more, and values still buffered in either channel go with the channels. A
 * coroutine that feeds `in` or reads `out` after the caller's own handles are gone holds a copy of its handle, not a
 * raw pointer: the handles are the only owners of the pipeline.
 */
template <typename T, typename U, typename F>
auto
make_pipeline(F f, size_t buffer_size = 10) {
    struct pipeline_lease { // what the handles share; `worker` is declared last, so it ends before the channels go
        std::shared_ptr<channel<T>> in;
        std::shared_ptr<channel<U>> out;
        detail::worker_lease        worker; // coroutine/utils.h
    };
    auto lease = std::make_shared<pipeline_lease>();
    lease->in  = std::make_shared<channel<T>>(buffer_size);
    lease->out = std::make_shared<channel<U>>(buffer_size);
    // The worker co-owns the channels and holds only its lease's slot; the two handles alias the lease, and own it alone.
    lease->worker.start(pipeline_worker<T, U>(std::move(f), lease->in, lease->out, lease->worker.slot()));
    return std::make_pair(std::shared_ptr<channel<T>>(lease, lease->in.get()), std::shared_ptr<channel<U>>(lease, lease->out.get()));
}

// Free function: fn, in, out and the slot of its lease are VALUE parameters stored in the coroutine frame, so nothing
// dangles once make_pipeline returns, and the channels are co-owned for the worker's whole lifetime.
template <typename T, typename U, typename F>
task<void>
pipeline_worker(F fn, std::shared_ptr<channel<T>> in, std::shared_ptr<channel<U>> out, std::shared_ptr<std::coroutine_handle<>> self = {}) {
    detail::worker_lease::slot_guard ends_the_slot{std::move(self)}; // first: clears the slot however this frame ends
    // Close `out` on EVERY exit: a transform (or a send) that throws ends the stream for the consumer, never strands it.
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
 * deliver a value (or close) resolves the state and wakes the caller.
 * The entries left in the other channels are retracted as the awaiter ends (Huly QB-273).
 */
template <typename... Ts>
class channel_select_awaiter {
    static constexpr char const *qb_suspension_kind = "channel select"; ///< suspension tracking (coroutine/tracking.h)

    using state_t = channel_select_state;
    state_t                                                _state{}; ///< owned here, by value: the channels' nodes point at it (Huly QB-273)
    std::tuple<channel<Ts> *...>                           _channels;
    std::tuple<typename channel<Ts>::select_entry...>      _entries{}; ///< one node per channel, linked into its list while parked
    std::array<std::shared_ptr<bool const>, sizeof...(Ts)> _alive{};   ///< each channel's liveness token, held from its registration

    // Pass 1: prefer any channel that has buffered data.
    template <size_t I>
    bool
    try_data() {
        auto *ch = std::get<I>(_channels);
        if (!ch->empty()) {
            if (auto val = ch->try_recv()) {
                _state.resolve(I, std::any(std::move(*val)));
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
            _state.resolve(I, std::any{}, true);
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

    // The token first, the registration second: should the registration's wake throw, the destructor still knows
    // this channel is alive and retracts the entry it may have queued (Huly QB-273).
    template <size_t... Is>
    void
    register_all(std::index_sequence<Is...>) {
        ((_alive[Is] = std::get<Is>(_channels)->liveness(),
          std::get<Is>(_channels)->register_select_waiter(std::get<Is>(_entries), _state, Is)),
         ...);
    }

    // The channels that did not win still hold an entry pointing at `_state`: take it back from each one still
    // alive (a destroyed channel closed first, which resolved the state and dropped its entries).
    template <size_t I>
    void
    unregister_one() noexcept {
        if (_alive[I] && *_alive[I])
            std::get<I>(_channels)->unregister_select_waiter(std::get<I>(_entries));
    }

    template <size_t... Is>
    void
    unregister_all(std::index_sequence<Is...>) noexcept {
        (unregister_one<Is>(), ...);
    }

public:
    explicit channel_select_awaiter(channel<Ts> &...chs)
        : _channels(&chs...) {}

    // Neither copyable nor movable (Huly QB-302): the destructor resolves the shared state, so a copy -- and a
    // "move", which was one (the user-declared destructor suppresses the implicit move) -- destroyed first made
    // every channel skip the live select, and its coroutine never resumed. `select` returns a prvalue
    // (guaranteed copy elision), so neither is needed: the rule of timeout_awaiter.
    channel_select_awaiter(const channel_select_awaiter &)            = delete;
    channel_select_awaiter(channel_select_awaiter &&)                 = delete;
    channel_select_awaiter &operator=(const channel_select_awaiter &) = delete;
    channel_select_awaiter &operator=(channel_select_awaiter &&)      = delete;

    // Whether this select is destroyed while still parked (cancelled before any channel resolved it) or after it
    // resumed: the entries still pointing at our state -- in the channels that did not win -- are retracted from
    // every channel still alive, then the state is closed so a later sender could not schedule a destroyed
    // coroutine handle even if an entry survived (it cannot, since Huly QB-273; before, the entries stayed until a
    // sender met and skipped them). Only the channels still alive are touched.
    //
    // NOTE (intentional, documented edge): unlike recv()/recv_for() — which give back a value handed
    // to a receiver that is then reclaimed — a multi-way select() that is RESOLVED with a value but
    // abandoned before resume (an outer when_any/race loser) DROPS that one value. Giving it back
    // would require a runtime-index→compile-time-type dispatch over the heterogeneous channel tuple;
    // and abandoning a whole multi-source select is semantically a choice-level abandon (you gave up
    // waiting on ALL of them), so dropping the single in-flight value is acceptable and consistent
    // across the variadic and vector select forms. Single-channel receives preserve the value; select
    // does not. If this ever matters, route the receive through recv()/recv_for() on one channel.
    ~channel_select_awaiter() {
        unregister_all(std::make_index_sequence<sizeof...(Ts)>{});
        _state.resolved = true;
        _state.outer    = {};
    }

    [[nodiscard]] bool
    await_ready() {
        return try_immediate();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        ::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);
        if (_state.resolved) {
            schedule_via_current(h);
            return;
        }
        _state.outer = h;
        register_all(std::make_index_sequence<sizeof...(Ts)>{});
    }

    select_result
    await_resume() {
        return {_state.winner, _state.closed, std::move(_state.value)};
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
    static constexpr char const *qb_suspension_kind = "channel select"; ///< suspension tracking (coroutine/tracking.h)

    using state_t = channel_select_state;
    /// A channel's node and liveness token, side by side: ONE vector, reserved before any registration, so the
    /// nodes never move while linked.
    struct slot {
        typename channel<T>::select_entry entry{};
        std::shared_ptr<bool const>       alive{};
    };
    state_t                   _state{}; ///< owned here, by value: the channels' nodes point at it (Huly QB-273)
    std::vector<channel<T> *> _channels;
    std::vector<slot>         _slots; ///< filled at the park: the one allocation this form keeps

public:
    explicit channel_select_vector_awaiter(std::vector<channel<T> *> chs)
        : _channels(std::move(chs)) {}

    // Neither copyable nor movable: see channel_select_awaiter (Huly QB-302).
    channel_select_vector_awaiter(const channel_select_vector_awaiter &)            = delete;
    channel_select_vector_awaiter(channel_select_vector_awaiter &&)                 = delete;
    channel_select_vector_awaiter &operator=(const channel_select_vector_awaiter &) = delete;
    channel_select_vector_awaiter &operator=(channel_select_vector_awaiter &&)      = delete;

    // See channel_select_awaiter::~channel_select_awaiter: the entries still pointing at our state are retracted
    // from every channel still alive, then the state is closed.
    ~channel_select_vector_awaiter() {
        for (size_t i = 0; i < _slots.size(); ++i)
            if (_slots[i].alive && *_slots[i].alive)
                _channels[i]->unregister_select_waiter(_slots[i].entry);
        _state.resolved = true;
        _state.outer    = {};
    }

    [[nodiscard]] bool
    await_ready() {
        // Pass 1: prefer data over close signals (avoids starvation).
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto *ch = _channels[i];
            if (!ch->empty()) {
                auto val = ch->try_recv();
                if (val) {
                    _state.resolve(i, std::any(std::move(*val)));
                    return true;
                }
            }
        }
        // Pass 2: report first closed-and-empty channel.
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto *ch = _channels[i];
            if (ch->is_closed() && ch->empty()) {
                _state.resolve(i, std::any{}, true);
                return true;
            }
        }
        return false;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        ::qb::io::async::detail::track_suspension(h.address(), qb_suspension_kind);
        if (_state.resolved) {
            schedule_via_current(h);
            return;
        }
        _state.outer = h;
        _slots.reserve(_channels.size()); // the one allocation: before any registration, so a throw here registers nothing
        for (size_t i = 0; i < _channels.size(); ++i) {
            auto *ch                    = _channels[i];
            _slots.emplace_back().alive = ch->liveness(); // the token first (Huly QB-273): a throwing wake below still finds it
            ch->register_select_waiter(_slots.back().entry, _state, i);
        }
    }

    select_result
    await_resume() {
        return {_state.winner, _state.closed, std::move(_state.value)};
    }
};

template <typename T>
auto
select(std::vector<channel<T> *> chs) {
    return channel_select_vector_awaiter<T>(std::move(chs));
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_CHANNEL_H
