/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2025 isndev (cpp.actor). All rights reserved.
 *
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
 */

#ifndef QB_IO_ASYNC_COROUTINE_MIXIN_H
#define QB_IO_ASYNC_COROUTINE_MIXIN_H

#include "task.h"

namespace qb::io::async {

/**
 * @brief CRTP Mixin to add coroutine support to existing command classes
 *
 * This mixin provides a unified interface for classes that want to expose
 * both synchronous and coroutine-based APIs. Use the coro() method to access
 * the coroutine interface.
 *
 * USAGE:
 * ======
 *
 * Add this as a base class to your command traits:
 *
 * @code
 * template <typename Derived>
 * class string_commands : public qb::io::async::coro_mixin<Derived> {
 *     // Existing sync/async methods...
 *
 *     std::string get(const std::string& key);
 *     Derived& get(Func&& func, const std::string& key);
 *
 *     // Coroutine methods are automatically available via CRTP!
 *     // Users can call: co_await client.coro().get("key");
 * };
 * @endcode
 *
 * For callback-to-coroutine bridging, use async_awaiter from awaiter.h:
 *
 * @code
 * task<std::string> fetch_data() {
 *     auto awaiter = async_awaiter<std::string>([](auto cb) {
 *         legacy_async_call([](const std::string& result) {
 *             cb(result);
 *         });
 *     });
 *     co_return co_await awaiter;
 * }
 * @endcode
 *
 * Finding 2.D.9 — relationship with qb::Actor::spawn_async:
 *   `coro_mixin` only exposes a `.coro()` accessor that turns an existing
 *   synchronous client into a coroutine-producing one; it does **not** own
 *   a scheduler and does not spawn tasks by itself. Inside an actor the
 *   returned task must still be driven through `Actor::spawn_async(...)`
 *   (or awaited from an already-spawned coroutine) so that the scheduler
 *   lifetime and the `active_coroutines_` counter are tracked correctly.
 *   Never call `run_sync()` on a task obtained through `.coro()` from an
 *   actor handler — it would block the VirtualCore thread.
 *
 * @tparam Derived The derived class (CRTP pattern)
 * @ingroup Coroutine
 * @see async_awaiter
 * @see qb::Actor::spawn_async
 */
template <typename Derived>
class coro_mixin {
protected:
    constexpr Derived& derived() noexcept {
        return static_cast<Derived&>(*this);
    }

public:
    /**
     * @brief Access coroutine-enabled interface
     *
     * Returns a proxy object that provides coroutine versions of all methods.
     * The proxy uses the same underlying client but returns task<T> instead of T.
     *
     * @example
     * auto result = co_await redis.coro().get("key");
     *
     * @return Reference to the derived type (CRTP pattern)
     */
    constexpr Derived& coro() noexcept {
        return derived();
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_MIXIN_H
