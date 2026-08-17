/**
 * @file qb/io/async/coroutine.h
 * @brief Main header for coroutine support in qb-io
 *
 * This header includes all coroutine-related functionality for the
 * QB async I/O framework. It provides C++20 coroutine integration
 * with the libev event loop.
 *
 * QUICK START:
 * ============
 *
 * Basic Usage:
 * @code
 * #include <qb/io/async.h>
 *
 * qb::io::async::task<int> fetch_data() {
 *     co_await qb::io::async::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * }
 *
 * int main() {
 *     qb::io::async::init();
 *     int result = 0;
 *     // spawn() takes a CALLABLE returning task<void> — pass the lambda itself, with no
 *     // trailing (), and read the value back through the enclosing scope. There is no
 *     // spawn(task<int>&&): the only task overload is task<void>, because a spawned
 *     // coroutine is detached and has nowhere to hand a value back to.
 *     qb::io::async::coro_scheduler().spawn([&result]() -> qb::io::async::task<void> {
 *         result = co_await fetch_data();
 *     });
 *     qb::io::async::run_for(std::chrono::seconds(1));
 *     return result == 42 ? 0 : 1;
 * }
 * @endcode
 *
 * CRITICAL SAFETY GUIDELINES:
 * ===========================
 *
 * 1. LAMBDA COROUTINE CAPTURE SAFETY (MOST COMMON ERROR)
 * -------------------------------------------------------
 * When creating coroutines from lambdas, temporary lambda objects create
 * dangling references after the first suspension point.
 *
 * ❌ WRONG - Temporary lambda with reference capture:
 * @code
 * auto t = [&data]() -> task<void> {
 *     co_await sleep(100ms);
 *     use(data);  // DANGLING REFERENCE! Lambda destroyed before coroutine runs
 * }();
 * @endcode
 *
 * ❌ WRONG - Loop variable captured by reference:
 * @code
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back([&i]() -> task<int> {
 *         co_await sleep(10ms);
 *         co_return i * 10;  // UNDEFINED: i out of scope or wrong value
 *     }());
 * }
 * @endcode
 *
 * ✅ CORRECT - Store lambda in variable:
 * @code
 * auto coro_fn = [&data]() -> task<void> {
 *     co_await sleep(100ms);
 *     use(data);
 * };
 * auto t = coro_fn();  // Lambda stays alive
 * @endcode
 *
 * ✅ CORRECT - Pass loop variable as parameter:
 * @code
 * auto worker = [](int id) -> task<int> {  // id passed by value
 *     co_await sleep(10ms);
 *     co_return id * 10;
 * };
 * for (int i = 0; i < 5; ++i) {
 *     tasks.push_back(worker(i));  // Safe: i copied into parameter
 * }
 * @endcode
 *
 * ✅ CORRECT - Capture by pointer to external data:
 * @code
 * auto data_ptr = &data;
 * auto coro_fn = [data_ptr]() -> task<void> {
 *     co_await sleep(100ms);
 *     use(*data_ptr);
 * };
 * auto t = coro_fn();
 * @endcode
 *
 * ✅ BEST - Use regular functions instead of lambdas:
 * @code
 * task<void> process_data(Data* data) {
 *     co_await sleep(100ms);
 *     use(*data);
 * }
 * @endcode
 *
 * 2. MOVE SEMANTICS
 * -----------------
 * task<T> is move-only. Always use std::move when passing to spawn():
 *
 * @code
 * auto t = my_coroutine();
 * coro_scheduler().spawn(std::move(t));  // ✅ Correct
 * coro_scheduler().spawn(t);             // ❌ Compile error
 * @endcode
 *
 * 3. AWAITING TASKS
 * -----------------
 * Tasks can be awaited to get their results:
 *
 * @code
 * task<int> get_value() {
 *     co_await sleep(100ms);
 *     co_return 42;
 * }
 *
 * task<void> caller() {
 *     int result = co_await get_value();  // Suspends until get_value completes
 *     std::cout << "Got: " << result << "\n";
 * }
 * @endcode
 *
 * 4. EXCEPTION HANDLING
 * ---------------------
 * Exceptions propagate through co_await:
 *
 * @code
 * task<int> may_throw() {
 *     co_await sleep(100ms);
 *     throw std::runtime_error("error");
 *     co_return 42;
 * }
 *
 * task<void> handler() {
 *     try {
 *         int result = co_await may_throw();
 *     } catch (const std::runtime_error& e) {
 *         std::cerr << "Caught: " << e.what() << "\n";
 *     }
 * }
 * @endcode
 *
 * COMMON PATTERNS:
 * ================
 *
 * Parallel Execution (Scatter-Gather):
 * @code
 * task<void> parallel_work() {
 *     auto worker = [](int id) -> task<int> {
 *         co_await sleep(std::chrono::milliseconds(10 * id));
 *         co_return id * 10;
 *     };
 *
 *     std::vector<task<int>> tasks;
 *     for (int i = 0; i < 5; ++i) {
 *         tasks.push_back(worker(i));  // Create tasks
 *     }
 *
 *     std::vector<int> results;
 *     for (auto& t : tasks) {
 *         results.push_back(co_await t);  // Await each
 *     }
 * }
 * @endcode
 *
 * Pipeline Processing:
 * @code
 * task<int> stage1(int x) {
 *     co_await sleep(10ms);
 *     co_return x * 2;
 * }
 *
 * task<int> stage2(int x) {
 *     co_await sleep(10ms);
 *     co_return x + 10;
 * }
 *
 * task<void> pipeline() {
 *     int result = co_await stage1(5);   // result = 10
 *     result = co_await stage2(result);  // result = 20
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
 * @defgroup Coroutine Coroutine Support
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_COROUTINE_H
#define QB_IO_ASYNC_COROUTINE_H

/**
 * @brief Main header for coroutine support in qb-io
 *
 * This header includes all coroutine-related functionality for the
 * QB async I/O framework. It provides C++20 coroutine integration
 * with the libev event loop.
 *
 * Include order (do not reorder):
 * 1. scheduler.h - CoroutineScheduler (no dependencies on task)
 * 2. task.h - task<T> (needs schedule_via_current from scheduler.h)
 * 3. awaiter.h - Awaiters (needs scheduler)
 * 4. utils.h - Utility functions (needs awaiters)
 * 5. mixin.h - CRTP mixin (optional, needs task)
 *
 * Single-thread: one scheduler per thread; do not share coroutine objects across threads.
 *
 * TCP coroutine connector is available in tcp/connector.h (when __cpp_impl_coroutine is defined).
 * (stream, client moved to coroutine/old/ - disabled until redesign)
 */

#include "coroutine/scheduler.h" // Must be first - defines schedule_via_current
#include "coroutine/task.h"      // Needs schedule_via_current for await_suspend
#include "coroutine/awaiter.h"   // Needs scheduler
#include "coroutine/utils.h"     // Needs awaiters
#include "coroutine/mixin.h"     // Optional CRTP mixin

// Combinators and utilities
#include "coroutine/combinators.h"  // when_all, when_any, race, timeout
#include "coroutine/cancellation.h" // cancellation_token, cancellable operations
#include "coroutine/channel.h"      // channel<T> for coroutine communication
#include "coroutine/sync.h"         // semaphore, async_mutex, barrier
#include "coroutine/retry.h"        // with_retry, retry policies
#include "coroutine/scope.h"        // coroutine_scope, lifetime management
#include "coroutine/generator.h"    // generator<T> with co_yield
#include "coroutine/stream.h"       // async_stream<T> transformations
#include "coroutine/shared_task.h"  // shared_task<T> — multi-consumer result

/**
 * @namespace qb::io::async
 * @brief Async I/O and coroutine support
 *
 * The qb::io::async namespace provides C++20 coroutine integration
 * with the libev event loop, enabling linear async code without callbacks.
 */

/**
 * @defgroup Coroutine Coroutine Support
 * @brief C++20 coroutines for QB async I/O
 *
 * This module provides coroutine support for the QB framework,
 * enabling `co_await`, `co_return`, and `co_yield` in async operations.
 *
 * ## Key Features
 *
 * - **task<T>**: Coroutine return type
 * - **Awaiters**: timer_awaiter, socket_awaiter for I/O operations
 * - **Scheduler**: CoroutineScheduler manages coroutine lifecycle
 * - **Integration**: Works with libev event loop
 *
 * ## Basic Usage
 *
 * @code
 * #include <qb/io/async.h>
 *
 * qb::io::async::task<int> compute() {
 *     co_await qb::io::async::sleep(std::chrono::seconds(1));
 *     co_return 42;
 * }
 *
 * int main() {
 *     qb::io::async::init();
 *     int answer = 0;
 *     // The lambda is passed WITHOUT a trailing () — spawn() owns the callable and builds
 *     // the frame itself. `spawn(compute())` would not compile: the only task overload is
 *     // spawn(task<void>&&), since a detached coroutine has nowhere to return a value.
 *     qb::io::async::coro_scheduler().spawn([&answer]() -> qb::io::async::task<void> {
 *         answer = co_await compute();
 *     });
 *     qb::io::async::run();
 *     return answer == 42 ? 0 : 1;
 * }
 * @endcode
 *
 * ## Actor Integration
 *
 * When using with qb-core Actors:
 *
 * @code
 * class MyActor : public qb::Actor {
 *     void on(Request& req) {
 *         // Capture by VALUE
 *         auto key = req.key;
 *         auto sender = req.sender;
 *
 *         spawn_detached([this, key, sender](auto ctx) -> qb::io::async::task<void> {
 *             auto result = co_await fetch(key);
 *             ctx.push<Result>(ctx.id(), sender, result);
 *         });
 *     }
 * };
 * @endcode
 *
 * ## Safety Guidelines
 *
 * 1. **Event handlers return void**: Never use `task<void> on(Event&)`
 * 2. **Capture by value**: No references/pointers to Actor members
 * 3. **Use ctx interface**: In spawn_detached, use ctx.push() not this->push()
 * 4. **No direct state access**: Communicate via events only
 *
 * @see task
 * @see timer_awaiter
 * @see socket_awaiter
 * @see CoroutineScheduler
 * @see Coroutine Safety
 */

/**
 * @page coroutine-safety Coroutine Safety
 * @brief Safety guidelines for coroutines in QB
 *
 * Coroutines can break Actor safety if misused. Follow these rules:
 *
 * ## The Problem
 *
 * @code
 * // ❌ WRONG - Breaks Actor safety
 * class BadActor : public qb::Actor {
 *     std::vector<Data> _buffer;
 *
 *     task<void> on(Request& req) {  // async handler = DANGER
 *         auto data = co_await fetch(req.key);  // SUSPENSION POINT
 *         _buffer.push_back(data);  // _buffer may have changed!
 *     }
 * };
 * @endcode
 *
 * When the coroutine suspends at `co_await`, other event handlers can run,
 * modifying `_buffer`. This is a data race even on a single thread.
 *
 * ## The Solution
 *
 * @code
 * // ✅ CORRECT - Isolated coroutine
 * class GoodActor : public qb::Actor {
 *     std::vector<Data> _buffer;
 *
 *     void on(Request& req) {  // sync handler = SAFE
 *         // Capture by VALUE
 *         auto key = req.key;
 *         auto sender = req.sender;
 *
 *         spawn_detached([this, key, sender](auto ctx) -> task<void> {
 *             // Runs in isolated context
 *             auto data = co_await fetch(key);
 *             ctx.push<Result>(ctx.id(), sender, data);  // via event
 *         });
 *     }
 *
 *     void on(Result& ev) {  // sync handler = SAFE again
 *         _buffer.push_back(ev.data);  // Exclusive access guaranteed
 *     }
 * };
 * @endcode
 *
 * ## The Rules
 *
 * 1. **Handlers return void**: `void on(Event&)` only
 * 2. **Spawn async for coroutines**: Use `spawn_detached()` for I/O
 * 3. **Capture by value**: Copy data into the lambda
 * 4. **Return via events**: `ctx.push<Event>()` only
 * 5. **Process in sync handler**: Handle results in `void on(Event&)`
 *
 * ## Why This Works
 *
 * - `spawn_detached()` runs coroutines in isolated context
 * - Coroutines can't access Actor state directly
 * - Communication is via events (Actor model)
 * - Results processed by sync handlers (exclusive access)
 *
 * @see Actor::spawn_detached()
 */

#endif // QB_IO_ASYNC_COROUTINE_H
