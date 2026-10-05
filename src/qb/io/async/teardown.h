/**
 * @file qb/io/async/teardown.h
 * @brief Run an io object's teardown hooks -- `on(event::disconnected&&)`, `on(event::dispose&&)` --
 *        with what they throw contained, so the teardown around them always completes.
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
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_TEARDOWN_H
#define QB_IO_ASYNC_TEARDOWN_H

#include <exception>
#include <utility>
#include <qb/io.h>                   /* QB_LOG_WARN */
#include <qb/utility/branch_hints.h> /* QB_NOINLINE, QB_COLD */

namespace qb::io::async::detail {

/**
 * @brief Run one teardown hook of an io object, containing what it throws.
 * @details Every `dispose()` -- `input<>`, `output<>`, `io<>`, `buffered_io<>` -- runs the derived
 *          class's `on(event::disconnected&&)` and then steps nothing may skip: it stops a standalone
 *          object's watcher, or hands a session back to its server, and fires `on(event::dispose&&)`.
 *          A hook that threw skipped them all. The listener contains the exception, so nothing crashed,
 *          and the object was left half torn down: its watcher still armed on a disposed io, which a
 *          readable fd (a client waiting in an acceptor's backlog, an EOF) then dispatched on every
 *          pass while `dispose()` returned at once -- a loop spinning on nothing -- or a session never
 *          handed back to its server (Huly QB-256). The exception is contained here, logged with its
 *          text, and the teardown goes on. `noexcept` is also what makes a `dispose()` reached from
 *          `disconnect_now()`, itself `noexcept`, safe from a throwing hook.
 *
 *          Out of line and cold: a teardown runs once per connection, and the hook's code, inlined into
 *          this frame, stays out of the io dispatch that reaches it.
 * @param hook The hook's name, for the log line.
 * @param run  A lambda written inside the base's member function that calls the hook: a lambda keeps
 *             the access of the function it is written in, so a handler its class declares private and
 *             exposes to the base still binds.
 */
template <typename Run>
QB_NOINLINE QB_COLD void
run_teardown_hook([[maybe_unused]] char const *hook, Run &&run) noexcept {
    try {
        std::forward<Run>(run)();
    } catch ([[maybe_unused]] std::exception const &e) {
        QB_LOG_WARN("[qb-io] " << hook << " threw: " << e.what() << " -- contained, the teardown completes");
    } catch (...) {
        QB_LOG_WARN("[qb-io] " << hook << " threw a non-std exception -- contained, the teardown completes");
    }
}

} // namespace qb::io::async::detail

#endif // QB_IO_ASYNC_TEARDOWN_H
