/**
 * @file qb/core/patterns/saga.h
 * @brief Saga orchestration: a sequence of steps with reverse-order compensation on failure.
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

#ifndef QB_CORE_PATTERNS_SAGA_H
#define QB_CORE_PATTERNS_SAGA_H

#include <cstddef>
#include <exception>
#include <functional>
#include <utility>
#include <vector>
#include <qb/core/Actor.h>
#include <qb/io/async/coroutine.h>

namespace qb {

/**
 * @class SagaScope
 * @ingroup Patterns
 * @brief Compensation stack for `qb::run_saga`.
 * @details A saga runs a sequence of steps, each of which may register a **compensation** — an
 *          async action that undoes the step. If a later step fails, `run_saga` runs the
 *          registered compensations in **reverse** order before re-throwing. A compensation is any
 *          callable returning `task<void>` (typically another `qb::ask(...)`).
 * @see qb::run_saga
 */
class SagaScope {
    std::vector<std::function<qb::io::async::task<void>()>> _compensations;

public:
    /**
     * @brief Register a compensation to undo the step that just succeeded.
     * @tparam Comp Callable returning `qb::io::async::task<void>`.
     * @param comp Runs (in reverse registration order) only if a later step fails.
     */
    template <typename Comp>
    void
    on_compensate(Comp &&comp) {
        _compensations.emplace_back(std::forward<Comp>(comp));
    }

    /** @brief Number of compensations registered so far (run LIFO on failure). */
    [[nodiscard]] std::size_t
    pending() const noexcept {
        return _compensations.size();
    }

    /**
     * @brief Run all registered compensations in reverse order, then clear them.
     * @details Best-effort: an exception from one compensation is swallowed so the remaining
     *          rollbacks still run. Invoked automatically by `run_saga` on failure.
     */
    qb::io::async::task<void>
    compensate() {
        while (!_compensations.empty()) {
            auto comp = std::move(_compensations.back());
            _compensations.pop_back();
            try {
                co_await comp();
            } catch (...) {
                // best-effort rollback: keep unwinding the remaining compensations.
            }
        }
    }
};

/** @brief Alias for `SagaScope`. */
using saga_scope = SagaScope;

/**
 * @brief Run a saga: a sequence of steps with automatic compensation on failure.
 * @ingroup Patterns
 * @tparam Body Callable `qb::io::async::task<void>(ScopedCoroContext, SagaScope&)` describing the
 *              forward steps; register an undo for each with `saga.on_compensate(...)`.
 * @param ctx The coroutine context (passed through to the body).
 * @param body The saga body.
 * @return `task<void>` completing when all steps succeed.
 * @throws Whatever a step throws — but only **after** the already-registered compensations have run
 *         in reverse order. A `cancelled_error` (actor killed) is re-thrown **without** compensation.
 * @code
 * co_await qb::run_saga(ctx, [inv, pay](qb::ScopedCoroContext ctx, qb::SagaScope &saga)
 *                            -> qb::io::async::task<void> {
 *     co_await qb::ask(ctx, inv, Reserve{item}, 1s);
 *     saga.on_compensate([ctx, inv, item]() -> qb::io::async::task<void> {
 *         co_await qb::ask(ctx, inv, Release{item}, 1s);     // undo the reserve
 *     });
 *     co_await qb::ask(ctx, pay, Charge{amount}, 1s);        // throws -> Release runs
 * });
 * @endcode
 * @see SagaScope, qb::ask
 */
template <typename Body>
[[nodiscard]] qb::io::async::task<void>
run_saga(qb::ScopedCoroContext ctx, Body body) {
    SagaScope          saga;
    std::exception_ptr failure;
    try {
        co_await body(ctx, saga);
        co_return; // every step succeeded — nothing to compensate.
    } catch (const qb::io::async::cancelled_error &) {
        throw; // actor is being killed — abort hard, without rollback.
    } catch (...) {
        // `co_await` is illegal inside a catch handler, so capture and leave it.
        failure = std::current_exception();
    }
    // Non-cancellation failure: roll back the registered steps, then propagate.
    co_await saga.compensate();
    std::rethrow_exception(failure);
}

} // namespace qb

#endif // QB_CORE_PATTERNS_SAGA_H
