/**
 * @file qb/core/patterns/routing.h
 * @brief `WorkerPool` — distribute work across a pool of worker actors (no coroutine needed).
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

#ifndef QB_CORE_PATTERNS_ROUTING_H
#define QB_CORE_PATTERNS_ROUTING_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <qb/core/ActorId.h>

namespace qb {

/**
 * @class WorkerPool
 * @ingroup Patterns
 * @brief Distributes work across a pool of worker actors.
 * @details A small, allocation-light helper holding a list of worker `ActorId`s and a round-robin
 *          cursor. The actor picks a worker and `push`es to it: `push<Work>(pool.next(), ...)`
 *          (round-robin), `push<Work>(pool.for_key(k), ...)` (sticky by key), or iterate
 *          `workers()` to broadcast. It does not own the workers or track their liveness — pair it
 *          with discovery/supervision if workers come and go.
 * @code
 * qb::WorkerPool pool{ {w0, w1, w2} };
 * push<Job>(pool.next(), job);              // round-robin
 * push<Session>(pool.for_key(userId), s);   // same user -> same worker
 * @endcode
 */
class WorkerPool {
    std::vector<qb::ActorId> _workers;
    std::size_t              _cursor = 0;

public:
    WorkerPool() = default;
    explicit WorkerPool(std::vector<qb::ActorId> workers)
        : _workers(std::move(workers)) {}

    /** @brief Append a worker to the pool. */
    void
    add(qb::ActorId worker) {
        _workers.push_back(worker);
    }
    /** @brief Remove a worker from the pool (if present). */
    void
    remove(qb::ActorId worker) {
        _workers.erase(std::remove(_workers.begin(), _workers.end(), worker), _workers.end());
        if (!_workers.empty())
            _cursor %= _workers.size();
    }
    [[nodiscard]] bool
    empty() const noexcept {
        return _workers.empty();
    }
    [[nodiscard]] std::size_t
    size() const noexcept {
        return _workers.size();
    }
    [[nodiscard]] const std::vector<qb::ActorId> &
    workers() const noexcept {
        return _workers;
    }

    /** @brief Round-robin selection (cycles through the pool). Precondition: `!empty()`. */
    [[nodiscard]] qb::ActorId
    next() noexcept {
        assert(!_workers.empty() && "WorkerPool::next() on an empty pool");
        auto const w = _workers[_cursor];
        _cursor      = (_cursor + 1) % _workers.size();
        return w;
    }

    /**
     * @brief Sticky selection by key: the same key always maps to the same worker (until the pool
     *        size changes). Precondition: `!empty()`.
     */
    [[nodiscard]] qb::ActorId
    for_key(std::uint64_t key) const noexcept {
        assert(!_workers.empty() && "WorkerPool::for_key() on an empty pool");
        return _workers[key % _workers.size()];
    }
};

/** @brief Alias for `WorkerPool`. */
using worker_pool = WorkerPool;

} // namespace qb

#endif // QB_CORE_PATTERNS_ROUTING_H
