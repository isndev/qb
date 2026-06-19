/**
 * @file qb/io/async/coroutine/utils.h
 * @brief Utility functions for coroutine operations
 *
 * Convenience functions for common coroutine await operations.
 *
 * QUICK START GUIDE:
 * ==================
 *
 * 1. Create a coroutine:
 * @code
 * qb::io::async::task<int> fetch_data() {
 *     co_await qb::io::async::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * }
 * @endcode
 *
 * 2. Spawn it on the scheduler:
 * @code
 * auto t = fetch_data();
 * qb::io::async::coro_scheduler().spawn(std::move(t));
 * @endcode
 *
 * 3. Run the event loop:
 * @code
 * qb::io::async::run_for(std::chrono::seconds(1));
 * @endcode
 *
 * COMMON PATTERNS:
 * ================
 *
 * Delay/Sleep:
 * @code
 * co_await sleep(100ms);  // Suspend for 100ms
 * @endcode
 *
 * Wait for I/O:
 * @code
 * co_await wait_readable(fd);   // Wait until fd has data
 * co_await wait_writable(fd);   // Wait until fd can accept data
 * @endcode
 *
 * Spawn Child Coroutines:
 * @code
 * task<void> parent() {
 *     auto child = child_coroutine();
 *     coro_scheduler().spawn(std::move(child));
 *     co_await sleep(100ms);  // Continue parent execution
 * }
 * @endcode
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

#ifndef QB_IO_ASYNC_COROUTINE_UTILS_H
#define QB_IO_ASYNC_COROUTINE_UTILS_H

#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <qb/system/timestamp.h> // qb::duration
#include "awaiter.h"
#include "../listener.h"

namespace qb::io::async {

/**
 * @brief Suspend coroutine for a duration
 *
 * Creates a timer_awaiter that resumes the coroutine after the specified time.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> delay() {
 *     co_await qb::io::async::sleep(std::chrono::seconds(1));
 *     std::cout << "One second later\n";
 * }
 * @endcode
 *
 * @param duration The time to sleep
 * @return timer_awaiter that suspends until duration elapses
 * @ingroup Coroutine
 */
inline timer_awaiter
sleep(qb::duration duration) {
    // Use the listener's event loop, not the default loop
    // This ensures timers work with listener::current.run()
    return timer_awaiter{duration, listener::current.loop()};
}

/**
 * @brief Suspend until socket is readable
 *
 * Resumes the coroutine when the file descriptor has data available.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> read_data(int fd) {
 *     co_await qb::io::async::wait_readable(fd);
 *     // fd now has data available
 *     char buffer[1024];
 *     ssize_t n = read(fd, buffer, sizeof(buffer));
 * }
 * @endcode
 *
 * @param fd The file descriptor to wait on
 * @return socket_awaiter that suspends until readable
 * @ingroup Coroutine
 */
inline socket_awaiter
wait_readable(int fd) {
    // Use the listener's event loop for consistency with sleep()
    return socket_awaiter{fd, EV_READ, listener::current.loop()};
}

#if defined(_WIN32)
inline socket_awaiter
wait_readable(uintptr_t handle) {
    return socket_awaiter{handle, EV_READ, listener::current.loop()};
}
#endif

/**
 * @brief Suspend until socket is writable
 *
 * Resumes the coroutine when the file descriptor can accept data.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> write_data(int fd, const std::string& data) {
 *     co_await qb::io::async::wait_writable(fd);
 *     // fd now ready for writing
 *     write(fd, data.c_str(), data.size());
 * }
 * @endcode
 *
 * @param fd The file descriptor to wait on
 * @return socket_awaiter that suspends until writable
 * @ingroup Coroutine
 */
inline socket_awaiter
wait_writable(int fd) {
    // Use the listener's event loop for consistency with sleep()
    return socket_awaiter{fd, EV_WRITE, listener::current.loop()};
}

#if defined(_WIN32)
inline socket_awaiter
wait_writable(uintptr_t handle) {
    return socket_awaiter{handle, EV_WRITE, listener::current.loop()};
}
#endif

/**
 * @brief Suspend until socket is readable or writable
 *
 * Resumes when either condition is met.
 *
 * @param fd The file descriptor
 * @param events EV_READ, EV_WRITE, or EV_READ | EV_WRITE
 * @return socket_awaiter
 * @ingroup Coroutine
 */
inline socket_awaiter
wait_for_io(int fd, int events) {
    return socket_awaiter{fd, events, listener::current.loop()};
}

#if defined(_WIN32)
inline socket_awaiter
wait_for_io(uintptr_t handle, int events) {
    return socket_awaiter{handle, events, listener::current.loop()};
}
#endif

/**
 * @brief Get reference to the current thread's coroutine scheduler
 *
 * Returns the listener's scheduler so that spawn, timers, and run_ready()
 * all use the same scheduler and event loop. Creates the scheduler on
 * first access.
 *
 * Usage:
 * @code
 * qb::io::async::task<void> spawn_another() {
 *     auto other_task = some_other_coroutine();
 *     qb::io::async::coro_scheduler().spawn(std::move(other_task));
 *     co_return;
 * }
 * @endcode
 *
 * @return Reference to the listener's CoroutineScheduler
 * @ingroup Coroutine
 */
inline CoroutineScheduler &
coro_scheduler() {
    // Always use the listener's scheduler to ensure consistency
    // This creates the scheduler on first access and sets it as current
    return listener::current.coro_scheduler();
}

/**
 * @brief Run the event loop for a duration
 *
 * Convenience function to run the event loop with coroutine support.
 *
 * @param duration Maximum time to run
 * @ingroup Coroutine
 */
inline void
run_for(qb::duration duration) {
    ensure_not_inside_ready_drain("run_for()");
    auto end = std::chrono::steady_clock::now() + duration;
    // Process any already-queued coroutines (e.g. from spawn) before the timed loop
    if (listener::current.has_coro_scheduler()) {
        listener::current.coro_scheduler().run_ready();
    }
    while (std::chrono::steady_clock::now() < end) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            break;

        // Run event loop (listener::run does run_ready() after _loop.run())
        for (int i = 0; i < 16 /*max event loop drain iterations*/; ++i) {
            listener::current.run(EVRUN_NOWAIT);
            if (!listener::current.has_coro_scheduler() || !listener::current.coro_scheduler().has_ready()) {
                break;
            }
        }

        // Small yield so wall clock advances and timers can fire
        if (!listener::current.size() && (!listener::current.has_coro_scheduler() || !listener::current.coro_scheduler().has_ready())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Drain any remaining ready coroutines
    if (listener::current.has_coro_scheduler()) {
        listener::current.coro_scheduler().run_ready();
    }
}

/**
 * @brief Execute a coroutine synchronously (blocking until completion)
 *
 * Runs a coroutine to completion, blocking the current thread until it finishes.
 * Useful for bridging synchronous code (like test SetUp) with coroutine APIs.
 *
 * Usage:
 * @code
 * // In test SetUp:
 * void SetUp() override {
 *     async::init();
 *     if (!run_sync(redis.connect()))
 *         throw std::runtime_error("Connection failed");
 * }
 *
 * // With return value:
 * auto reply = run_sync(redis.get("key"));
 * @endcode
 *
 * @tparam Awaitable The awaitable type (task, connect_awaiter, etc.)
 * @param awaitable The awaitable to execute
 * @return The result of the awaitable
 * @ingroup Coroutine
 */
template <typename Awaitable>
auto
run_sync(Awaitable &&awaitable)
    -> std::conditional_t<std::is_void_v<decltype(std::declval<std::remove_cvref_t<Awaitable>>().await_resume())>, void,
                          std::remove_cvref_t<decltype(std::declval<std::remove_cvref_t<Awaitable>>().await_resume())>> {
    ensure_not_inside_ready_drain("run_sync()");
    using raw_awaitable = std::remove_cvref_t<Awaitable>;
    using return_type   = decltype(std::declval<raw_awaitable>().await_resume());
    using value_type    = std::remove_cvref_t<return_type>;

    // Capture awaitable by reference: some awaiters are non-movable (e.g. redis_awaiter holds
    // Reply with deleted copy/move). Caller prvalues (run_sync(redis.flushall())) live until
    // run_sync returns, so the reference is valid for the pump loop.
    // Return type is explicit value (not decltype(auto)): `return std::move(*optional)` can deduce
    // an rvalue reference into the optional's storage and dangle after this function returns.
    auto pump = [](bool &done) {
        while (!done) {
            bool has_ready = listener::current.has_coro_scheduler() && listener::current.coro_scheduler().has_ready();
            if (has_ready) {
                for (int i = 0; i < 16 && !done; ++i) {
                    listener::current.run(EVRUN_NOWAIT);
                    if (!listener::current.has_coro_scheduler() || !listener::current.coro_scheduler().has_ready())
                        break;
                }
            } else {
                // Do not use EVRUN_ONCE here: when libev uses timerfd-based time-jump
                // detection, waittime can become MAX_BLOCKTIME2 while timercnt==0 (only
                // ev_io watchers). One EVRUN_ONCE then blocks epoll_wait for ~10^6 seconds,
                // freezing run_sync() and any test SetUp/TearDown that uses it.
                listener::current.run(EVRUN_NOWAIT);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };

    if constexpr (std::is_void_v<return_type>) {
        bool               done = false;
        std::exception_ptr error;
        coro_scheduler().spawn([&awaitable, &done, &error]() -> task<void> {
            try {
                co_await awaitable;
            } catch (...) {
                error = std::current_exception();
            }
            done = true;
        });
        pump(done);
        if (error) {
            std::rethrow_exception(error);
        }
    } else {
        std::optional<value_type> result;
        bool                      done = false;
        std::exception_ptr        error;
        coro_scheduler().spawn([&awaitable, &result, &done, &error]() -> task<void> {
            try {
                result = co_await awaitable;
            } catch (...) {
                error = std::current_exception();
            }
            done = true;
        });
        pump(done);
        if (error) {
            std::rethrow_exception(error);
        }
        return std::move(*result);
    }
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_COROUTINE_UTILS_H
