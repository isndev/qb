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

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>
#include <qb/io.h>                    /* QB_LOG_WARN */
#include <qb/io/system/sys__socket.h> /* qb::io::invalid_socket, QB_SD_SEND, ::shutdown */
#include <qb/utility/branch_hints.h>  /* QB_NOINLINE, QB_COLD */
#include <qb/utility/type_traits.h>   /* qb::has_disconnect */

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

/**
 * @brief End the transport of a standalone io object's connection, once its watcher is stopped.
 * @details A standalone object -- a client, an acceptor: no server owns it -- keeps its transport for as long
 *          as it lives, and its `dispose()` stopped the watcher and left the transport as it was: after a
 *          `disconnect()`, a framing error or the peer's EOF the connection stayed open until the object
 *          connected again or was destroyed. The peer of a `disconnect()` never saw EOF, so a server kept the
 *          connection and the work it routed for it alive, and a client whose peer hung up stayed in CLOSE_WAIT
 *          (Huly QB-1060; qbm-http's HTTP/2 client met it as QB-1021). Every standalone `dispose()` calls this
 *          after `on(event::disconnected&&)`, which still sees the transport as the connection left it, and
 *          before `on(event::dispose&&)`, the one hook that may free the object; the watcher is stopped first,
 *          since libev must not watch a descriptor closed under it. How depends on who ended the connection:
 *
 *          - The peer or the socket (`ended_by_peer`): its EOF, a read or a write error. Nothing of ours can be
 *            lost -- the peer sends nothing more and what it sent was read -- so the transport is closed
 *            outright: `disconnect()` when it has one, as `stream::close()` does (the TLS socket clears its
 *            connected state there), then `close()`, which releases the descriptor.
 *          - This object: `disconnect()`, a framing error, a size guard, the end of `close_after_deliver()`.
 *            Only the sending side is shut down: the peer reads EOF once everything queued has left, and
 *            nothing is reset. Closing here would reset a peer whose bytes are still unread: its next send
 *            fails, and on Windows it also drops what it received and has not read yet -- the tail of the very
 *            reply `close_after_deliver()` promised. The descriptor stays with the object, as it did, until its
 *            destruction or its next connection's transport releases it. A listening socket has no sending
 *            side: an acceptor keeps its port until then.
 *
 *          Both are no-ops on a closed socket, so a hook that closes the transport again does nothing. A
 *          transport whose handle is not a socket is not shut down; one without `close()` is not closed.
 *          Out of line and cold, like the hooks around it.
 * @param transport     The object's transport (`Derived.transport()`).
 * @param ended_by_peer Whether the peer's EOF or a socket error ended the connection, rather than this object.
 */
template <typename Transport>
QB_NOINLINE QB_COLD void
end_transport(Transport &transport, bool ended_by_peer) noexcept {
    if (ended_by_peer) {
        if constexpr (qb::has_disconnect<Transport>)
            static_cast<void>(transport.disconnect());
        if constexpr (requires { transport.close(); })
            transport.close();
        return;
    }
    using handle_type = std::remove_cv_t<decltype(qb::io::invalid_socket)>;
    if constexpr (requires {
                      { transport.native_handle() } -> std::same_as<handle_type>;
                  }) {
        const handle_type handle = transport.native_handle();
        if (handle != qb::io::invalid_socket)
            static_cast<void>(::shutdown(handle, QB_SD_SEND)); // the FIN leaves after what is queued
    }
}

} // namespace qb::io::async::detail

#endif // QB_IO_ASYNC_TEARDOWN_H
