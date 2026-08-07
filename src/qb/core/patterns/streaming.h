/**
 * @file qb/core/patterns/streaming.h
 * @brief Multi-reply streaming over the actor mailbox (`qb::ask_stream`).
 *
 * The `ask` pattern is single-reply. `ask_stream` lets a responder send **many** chunks for one
 * request: the asker `co_await`s them one at a time through a `qb::stream<E>` until the responder
 * signals end-of-stream. Chunks are `AskEvent`s routed through the same per-core continuation
 * registry as `ask` (a stable stream id reusing `AskEvent::correlation_id`), so a stream works even
 * while the asker is still *Activating* (inside `onInit`). Core-local (single thread): no locking.
 *
 * Protocol: the asker sends the request (its `correlation_id` is the stream id); the responder
 * pushes data chunks back with `qb::yield_answer(self, request, chunk)` and finishes with
 * `qb::end_stream(self, request)`. The asker routes every reply via `resolve_ask(e)` (chunks are
 * `AskEvent`s) and drains them with `co_await stream.next()` (yields `std::nullopt` at end-of-stream).
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
 * @ingroup Patterns
 */

#ifndef QB_CORE_PATTERNS_STREAMING_H
#define QB_CORE_PATTERNS_STREAMING_H

#include <concepts>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <qb/core/Actor.h>
#include <qb/io/async/coroutine.h> // cancellation_token / timeout_error / schedule_via_current
#include "request.h"               // qb::detail::ask_next_id / ask_loop / to_ev_seconds reuse

namespace qb {

/**
 * @struct StreamRequest
 * @ingroup Patterns
 * @brief Base exchange event for `ask_stream`: a request that round-trips as a stream of `Chunk`s.
 * @tparam Chunk The per-reply payload type.
 * @details Derive your exchange from `StreamRequest<Chunk>` and add **request** fields; the base
 *          supplies the `chunk` slot, the `stream_done` end marker, and the `AskEvent` stream id.
 * @code
 * struct Tail : qb::StreamRequest<LogLine> { qb::string<64> file; }; // qb::string, not std::string
 * @endcode
 */
template <class Chunk>
struct StreamRequest : qb::AskEvent {
    using chunk_type = Chunk;  ///< per-reply payload type
    Chunk chunk{};             ///< filled per reply by the responder (`yield_answer`)
    bool  stream_done = false; ///< set by `end_stream` to mark end-of-stream (no payload)
};

/** @brief Alias template for `StreamRequest`. */
template <class Chunk>
using stream_request = StreamRequest<Chunk>;

/**
 * @concept stream_event_type
 * @ingroup Patterns
 * @brief A `StreamRequest`-style event: derives from `AskEvent`, copyable, with `chunk`/`stream_done`.
 * @details Copyable (like `ask_event_type`): each chunk is carried by a copy of the event, so the
 *          `chunk_type` payload must itself be copyable. A move-only exchange is rejected as a clear
 *          concept error rather than a deep template failure.
 */
template <class E>
concept stream_event_type = std::derived_from<E, qb::AskEvent> && std::copyable<E> && requires(E e) {
    typename E::chunk_type;
    e.chunk;
    e.stream_done;
};

/**
 * @struct stream_overflow_error
 * @ingroup Patterns
 * @brief Thrown by `stream::next()` if the responder outran the stream buffer (chunks were dropped).
 */
struct stream_overflow_error : std::runtime_error {
    stream_overflow_error()
        : std::runtime_error("qb::ask_stream: buffer overflow — responder outpaced the consumer") {}
};

namespace detail {

/**
 * @brief Per-stream shared state: a FIFO chunk queue + one parked consumer.
 * @details Registers a **multi-shot** `ask_slot` in the per-core continuation registry (shared with
 *          ask), so chunks are delivered uniformly whether the asker is active OR still *Activating*
 *          (the activation gate routes correlated replies through that registry). Its deliver thunk
 *          never sets `slot.done`, so every chunk keeps arriving until the stream deregisters.
 *          Single-producer/single-consumer, single-thread: no locking.
 */
template <class E>
struct stream_state {
    std::deque<E>                     queue;           ///< chunks awaiting consumption (bounded by `cap`)
    std::size_t                       cap;             ///< max buffered chunks before overflow
    bool                              done    = false; ///< end_stream received (or terminal overflow)
    bool                              dropped = false; ///< a chunk overflowed the buffer
    qb::io::async::cancellation_token token;           ///< actor scope (a kill wakes the consumer)
    std::coroutine_handle<>           waiter{};        ///< the single parked `next()` awaiter (or null)
    qb::detail::ask_slot              slot{};          ///< entry in the continuation registry

    explicit stream_state(std::size_t capacity)
        : cap(capacity) {}

    void
    wake() noexcept {
        if (waiter)
            qb::io::async::schedule_via_current(std::exchange(waiter, std::coroutine_handle<>{}));
    }
    void
    deliver(E e) {
        if (done)
            return; // already terminal — drop late chunks
        if (queue.size() >= cap) {
            dropped = true; // overflow → fail the stream loudly (next() throws), no silent drop
            done    = true;
            wake();
            return;
        }
        queue.emplace_back(std::move(e));
        wake();
    }
    void
    finish() noexcept {
        done = true; // end-of-stream; buffered chunks still drain before next() yields nullopt
        wake();
    }
    // Continuation-registry callback: a reply (chunk or end marker) for this stream arrived.
    static void
    deliver_thunk(void *self, qb::Event &ev) noexcept {
        auto *st = static_cast<stream_state<E> *>(self);
        auto &e  = static_cast<E &>(ev); // E derives CorrelatedEvent; type registered ⇒ safe
        if (e.stream_done)
            st->finish();
        else
            st->deliver(std::move(e));
    }
};

/**
 * @brief Awaiter backing `stream::next()` — wakes on a chunk, end-of-stream, per-chunk timeout
 *        (its own `qev_timer`) or actor-scope cancel. Mirrors `qb::detail::ask_awaiter`.
 */
template <class E>
struct stream_next_awaiter {
    std::shared_ptr<stream_state<E>>           st;
    qb::duration                               timeout;
    qev_timer                                   timer{};
    bool                                       timer_started = false;
    bool                                       timed_out     = false;
    std::shared_ptr<bool>                      alive         = std::make_shared<bool>(true);
    qb::io::async::cancellation_token::id_type cancel_id     = 0;
    std::coroutine_handle<>                    parked{}; ///< handle we stored in st->waiter (cleared on teardown)

    stream_next_awaiter(std::shared_ptr<stream_state<E>> s, qb::duration t)
        : st(std::move(s))
        , timeout(t) {}
    stream_next_awaiter(const stream_next_awaiter &)            = delete;
    stream_next_awaiter &operator=(const stream_next_awaiter &) = delete;

    [[nodiscard]] bool
    await_ready() const noexcept {
        return !st->queue.empty() || st->done || st->token.is_cancelled();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        st->waiter = h;
        parked     = h;
        if (timeout.count() > 0) {
            qev_timer_init(&timer, &stream_next_awaiter::on_timeout, qb::detail::to_ev_seconds(timeout), 0.0);
            timer.data = this;
            auto loop  = qb::detail::ask_loop();
            qev_now_update(static_cast<struct qev_loop *>(loop));
            qev_timer_start(loop, &timer);
            timer_started = true;
        }
        auto a    = alive;
        cancel_id = st->token.on_cancel([this, a]() {
            if (*a)
                st->wake(); // await_resume will see token cancelled and throw
        });
    }

    std::optional<E>
    await_resume() {
        stop_timer();
        st->token.remove_on_cancel(cancel_id);
        cancel_id  = 0;
        st->waiter = {};
        if (!st->queue.empty()) {
            E v = std::move(st->queue.front());
            st->queue.pop_front();
            return v; // a chunk (drained even if the stream is already done)
        }
        if (st->dropped)
            throw stream_overflow_error{};
        if (st->token.is_cancelled())
            throw qb::io::async::cancelled_error{};
        if (st->done)
            return std::nullopt; // graceful end-of-stream
        if (timed_out)
            throw qb::io::async::timeout_error{};
        return std::nullopt; // unreachable in practice
    }

    ~stream_next_awaiter() {
        if (alive)
            *alive = false;
        stop_timer();
        st->token.remove_on_cancel(cancel_id);
        // Destroyed while still parked (await_resume never ran — e.g. a when_any/race loser reclaim
        // where the `stream` is owned in an outer frame that keeps the registry slot alive): clear
        // our handle from the shared state so a later chunk's deliver_thunk → st->wake() does not
        // schedule_via_current() our freed frame. The held `st` keeps the state valid here. Guard on
        // `parked` so a normal-completion teardown (await_resume already nulled st->waiter, or a wake
        // std::exchange'd it) is a no-op.
        if (parked && st->waiter == parked)
            st->waiter = {};
    }

private:
    void
    stop_timer() noexcept {
        if (timer_started) {
            qev_timer_stop(qb::detail::ask_loop(), &timer);
            timer_started = false;
        }
    }
    static void
    on_timeout(struct qev_loop *, qev_timer *w, int) noexcept {
        auto *me = static_cast<stream_next_awaiter *>(w->data);
        if (me && !me->st->done && me->st->waiter) {
            me->timed_out = true;
            me->st->wake();
        }
    }
};

} // namespace detail

/**
 * @class stream
 * @ingroup Patterns
 * @brief A consumable handle to an `ask_stream` — drain chunks with `co_await next()`.
 * @tparam E The `stream_event_type` exchange event.
 * @details Move-only, single-consumer; deregisters itself on destruction so late chunks are dropped
 *          harmlessly. Do not call `next()` concurrently on the same `stream`.
 */
template <stream_event_type E>
class stream {
public:
    stream(std::uint64_t id, std::shared_ptr<detail::stream_state<E>> st, qb::duration timeout)
        : _id(id)
        , _st(std::move(st))
        , _timeout(timeout) {}

    stream(const stream &)            = delete;
    stream &operator=(const stream &) = delete;
    // Move transfers ownership: the moved-from `_st` is null, so only the new owner deregisters.
    stream(stream &&o) noexcept
        : _id(o._id)
        , _st(std::move(o._st))
        , _timeout(o._timeout) {}
    stream &operator=(stream &&) = delete;

    ~stream() {
        if (_st)                             // not moved-from
            qb::detail::ask_unregister(_id); // leave the continuation registry
    }

    /**
     * @brief Await the next chunk.
     * @return The next reply event (read `.chunk`), or `std::nullopt` at end-of-stream.
     * @throws qb::io::async::timeout_error if no chunk arrives within the per-chunk timeout.
     * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
     * @throws qb::stream_overflow_error if the responder outpaced the buffer (chunks were dropped).
     */
    [[nodiscard]] detail::stream_next_awaiter<E>
    next() {
        return detail::stream_next_awaiter<E>{_st, _timeout};
    }

private:
    std::uint64_t                            _id;
    std::shared_ptr<detail::stream_state<E>> _st;
    qb::duration                             _timeout;
};

/**
 * @brief Start a streaming request: send `req` to `target`, return a `stream` of its replies.
 * @ingroup Patterns
 * @tparam E The `stream_event_type` exchange event.
 * @param ctx The spawning coroutine's context (its scope cancels the stream on kill).
 * @param target The actor to ask.
 * @param req The request event (request fields set; the responder fills `chunk` per reply).
 * @param timeout Per-chunk timeout (`<= 0` waits indefinitely between chunks). Defaults to 5 s.
 * @param capacity Max chunks buffered ahead of the consumer (overflow ⇒ `stream_overflow_error`).
 * @return A `stream<E>`; drain it with `while (auto c = co_await s.next()) use(c->chunk);`.
 * @code
 * auto s = qb::ask_stream(ctx, tailer, Tail{.file="app.log"}, 1s);
 * while (auto line = co_await s.next()) print(line->chunk);
 * @endcode
 * @note Usable inside `onInit()` — chunks reach an *Activating* asker via the continuation registry
 *       (the activation gate routes correlated replies). The asker routes chunks with
 *       `resolve_ask(e)` in its `on(E&)` (chunks are `AskEvent`s).
 * @see qb::Actor::resolve_ask, qb::yield_answer, qb::end_stream
 */
template <stream_event_type E>
[[nodiscard]] stream<E>
ask_stream(qb::ScopedCoroContext ctx, qb::ActorId target, E req, qb::duration timeout = std::chrono::seconds{5}, std::size_t capacity = 256) {
    const std::uint64_t id = qb::detail::ask_next_id(ctx.id());
    auto                st = std::make_shared<detail::stream_state<E>>(capacity ? capacity : std::size_t{1});
    st->token              = ctx.token();
    // Register a multi-shot continuation slot so chunks are delivered uniformly (active or
    // Activating) by the same registry/gate as ask.
    st->slot.owner   = ctx.id();
    st->slot.done    = false;
    st->slot.self    = st.get();
    st->slot.deliver = &detail::stream_state<E>::deliver_thunk;
    qb::detail::ask_register(id, &st->slot);
    qb::detail::ask_register_type(qb::Event::template type_to_id<E>());
    qb::detail::ask_slot_guard guard{id}; // deregister if the send below throws (no dangling slot)

    req.correlation_id = id;
    ctx.template push_to<E>(target, std::move(req)); // send to target, source = asker
    guard.release();                                 // the stream now owns the slot (its dtor cleans up)
    return stream<E>{id, std::move(st), timeout};
}

/**
 * @brief Responder-side: push one data chunk back to the asker of `request`.
 * @ingroup Patterns
 * @tparam E The `stream_event_type` exchange event.
 * @param self The responding actor.
 * @param request The originally received request (its source + stream id are reused).
 * @param chunk The payload for this reply.
 */
template <stream_event_type E>
void
yield_answer(qb::Actor &self, E const &request, typename E::chunk_type chunk) {
    auto &ev          = self.template push<E>(request.getSource());
    ev.correlation_id = request.correlation_id;
    ev.chunk          = std::move(chunk);
    ev.stream_done    = false;
}

/**
 * @brief Responder-side: signal end-of-stream to the asker of `request` (no payload).
 * @ingroup Patterns
 * @tparam E The `stream_event_type` exchange event.
 * @param self The responding actor.
 * @param request The originally received request.
 */
template <stream_event_type E>
void
end_stream(qb::Actor &self, E const &request) {
    auto &ev          = self.template push<E>(request.getSource());
    ev.correlation_id = request.correlation_id;
    ev.stream_done    = true;
}

} // namespace qb

#endif // QB_CORE_PATTERNS_STREAMING_H
