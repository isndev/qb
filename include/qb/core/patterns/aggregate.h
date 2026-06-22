/**
 * @file qb/core/patterns/aggregate.h
 * @brief Size/time-windowed batching of items for an actor (`qb::batcher`).
 *
 * Coalesce a stream of small events into batches flushed either when `max` items accumulate or
 * when a time `window` elapses since the first buffered item — amortizing a costly per-item action
 * (a DB write, a network round-trip) over a whole batch. The window timer is **scope-bound** to the
 * actor (a kill cancels it), so a dead actor never runs the flush callback.
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

#ifndef QB_CORE_PATTERNS_AGGREGATE_H
#define QB_CORE_PATTERNS_AGGREGATE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <qb/core/Actor.h>            // qb::ScopedCoroContext
#include <qb/io/async/coroutine.h>   // coro_scheduler / task / cancelled_error

namespace qb {

/**
 * @class batcher
 * @ingroup Patterns
 * @brief Buffers items and flushes them as a batch on a **count** or **time** trigger.
 * @tparam T The buffered item type.
 * @details
 * Call `add(ctx, item)` from an actor handler; the batch flushes (your `on_flush(std::vector<T>&&)`
 * runs once with the whole vector) as soon as either `max` items are buffered or `window` elapses
 * since the first item of the current batch — whichever comes first. The time trigger is a
 * cancellation-aware coroutine bound to the actor scope (`ctx`), so a killed actor cancels the
 * pending flush (buffered items are dropped, not flushed — call `flush()` from a shutdown handler
 * if you need a final drain). Core-local (single thread): no locking.
 *
 * Hold it as an **actor member**. Unlike `CircuitBreaker`/`rate_limiter` (captured by value into a
 * coroutine), a `batcher` is used synchronously from handlers, and `on_flush` **may safely reference
 * the actor** (`[this]`): the scope-bound window timer guarantees the flush never fires after the
 * actor is gone.
 * @code
 * class Writer : public qb::Actor {
 *     qb::batcher<Row> _batch{128, 50ms, [this](std::vector<Row> &&rows){ db_write(std::move(rows)); }};
 * public:
 *     void on(Row &e) { _batch.add(context(), e.row); } // flush at 128 rows or 50ms, whichever first
 * };
 * @endcode
 */
template <class T>
class batcher {
    struct state {
        std::size_t                           max;
        qb::duration                          window;
        std::function<void(std::vector<T> &&)> on_flush;
        std::vector<T>                        buf;
        std::uint64_t                         gen         = 0;     ///< bumps on each flush — stale timers see a mismatch
        bool                                  timer_armed = false; ///< at most one window timer per batch

        void
        do_flush() {
            ++gen;
            timer_armed = false;
            if (buf.empty())
                return;
            std::vector<T> batch = std::move(buf);
            buf.clear(); // moved-from vector → defined but unspecified; clear to a known-empty state
            on_flush(std::move(batch));
        }
    };

public:
    /**
     * @brief Create a batcher.
     * @param max Flush when this many items are buffered (clamped to >= 1).
     * @param window Flush this long after the first item of a batch (`<= 0` ⇒ size-only, no timer).
     * @param on_flush Invoked with the whole batch on each flush.
     */
    batcher(std::size_t max, qb::duration window, std::function<void(std::vector<T> &&)> on_flush)
        : _s(std::make_shared<state>()) {
        _s->max      = max ? max : std::size_t{1};
        _s->window   = window;
        _s->on_flush = std::move(on_flush);
    }

    /**
     * @brief Buffer one item; flush if the count trigger is hit, else arm the window timer.
     * @param ctx The actor's context (scopes the window timer to the actor's lifetime).
     * @param item The item to buffer (moved).
     */
    void
    add(qb::ScopedCoroContext ctx, T item) {
        _s->buf.emplace_back(std::move(item));
        if (_s->buf.size() >= _s->max) {
            _s->do_flush();
            return;
        }
        if (!_s->timer_armed && _s->window > qb::duration::zero())
            arm_timer(ctx);
    }

    /** @brief Flush the current buffer now (no-op if empty); invalidates the pending window timer. */
    void
    flush() {
        _s->do_flush();
    }

    /** @brief Items currently buffered (not yet flushed). */
    [[nodiscard]] std::size_t
    pending() const noexcept {
        return _s->buf.size();
    }

private:
    void
    arm_timer(qb::ScopedCoroContext ctx) {
        _s->timer_armed = true;
        auto              s = _s;       // keep state alive while the timer is pending
        const std::uint64_t g = _s->gen; // the batch this timer guards
        // lambda-safe spawn (closure owned, no trailing `()`); ctx.sleep is cancel-on-kill.
        qb::io::async::coro_scheduler().spawn([s, g, ctx]() -> qb::io::async::task<void> {
            try {
                co_await ctx.sleep(s->window);
            } catch (const qb::io::async::cancelled_error &) {
                co_return; // actor killed → drop the timer; buffered items are not flushed
            }
            if (s->gen != g)
                co_return; // already flushed (count trigger or manual) → this timer is stale
            s->do_flush();
        });
    }

    std::shared_ptr<state> _s;
};

} // namespace qb

#endif // QB_CORE_PATTERNS_AGGREGATE_H
