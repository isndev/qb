/**
 * @file qb/core/patterns/scatter.h
 * @brief Scatter-gather over `qb::ask` — `ask_all` (await every reply) and `ask_any` (first wins).
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

#ifndef QB_CORE_PATTERNS_SCATTER_H
#define QB_CORE_PATTERNS_SCATTER_H

#include <any>
#include <vector>
#include <qb/io/async/coroutine.h>
#include "request.h"

namespace qb {

/**
 * @brief Scatter-gather: `ask` every target with a copy of `req`, await **all** replies.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask (each receives its own copy + correlation id).
 * @param req The request, copied per target.
 * @param timeout Per-ask timeout.
 * @return `task<std::vector<E>>` — one filled response envelope per target, in input order.
 * @throws qb::io::async::timeout_error if **any** target fails to reply in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Built on `when_all` over per-target `qb::ask`s — replicated reads, quorum inputs,
 *          fan-out queries. A cross-core scatter keeps N asks in flight at once (bounded by
 *          `timeout`).
 * @code
 * auto quotes = co_await qb::ask_all(ctx, markets, Quote{"BTC"}, 500ms);
 * for (auto const &q : quotes) use(q.response);
 * @endcode
 * @see qb::ask, qb::ask_any
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<std::vector<E>>
ask_all(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, E req, qb::duration timeout) {
    std::vector<qb::io::async::task<E>> calls;
    calls.reserve(targets.size());
    for (auto const target : targets)
        calls.emplace_back(qb::ask<E>(ctx, target, req, timeout));
    co_return co_await qb::io::async::when_all(std::move(calls));
}

/**
 * @brief Race: `ask` every target, resolve with the **first** reply; the others are abandoned.
 * @ingroup Patterns
 * @tparam E The exchange event type (an `ask_event_type`).
 * @param ctx The coroutine context.
 * @param targets The actors to ask.
 * @param req The request, copied per target.
 * @param timeout Shared timeout — if no reply arrives within it, throws `timeout_error`.
 * @return `task<E>` — the first responder's filled envelope.
 * @throws qb::io::async::timeout_error if no target replies in time.
 * @throws qb::io::async::cancelled_error if the actor is killed while waiting.
 * @details Built on `when_any`. The losing asks are **not** cancelled — they linger until their
 *          own `timeout` (bounded, harmless). Use it for "fastest replica wins" / hedged requests.
 * @see qb::ask, qb::ask_all
 */
template <ask_event_type E>
[[nodiscard]] qb::io::async::task<E>
ask_any(qb::ScopedCoroContext ctx, std::vector<qb::ActorId> targets, E req, qb::duration timeout) {
    std::vector<qb::io::async::task<E>> calls;
    calls.reserve(targets.size());
    for (auto const target : targets)
        calls.emplace_back(qb::ask<E>(ctx, target, req, timeout));
    auto winner = co_await qb::io::async::when_any(std::move(calls));
    co_return std::any_cast<E>(std::move(winner.second));
}

} // namespace qb

#endif // QB_CORE_PATTERNS_SCATTER_H
