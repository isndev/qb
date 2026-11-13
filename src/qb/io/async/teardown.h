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
#include <qb/io.h>                          /* QB_LOG_WARN */
#include <qb/io/async/event/disconnected.h> /* event::disconnect_reason */
#include <qb/io/async/lingering_close.h>    /* detail::close_after_peer, detail::default_linger */
#include <qb/io/system/sys__socket.h>       /* qb::io::invalid_socket, QB_SD_SEND, ::shutdown */
#include <qb/system/time.h>                 /* qb::duration */
#include <qb/utility/branch_hints.h>        /* QB_NOINLINE, QB_COLD */
#include <qb/utility/type_traits.h>         /* qb::has_disconnect */

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

/// The bound that says "this transport is not closed after its peer": a datagram transport, kept as it is. Only
/// `close_bound()` makes it: an object's own linger is never read as it, however negative.
inline constexpr qb::duration keep_descriptor{-1};

/**
 * @brief The bound of the close of a connection its own side ended (`end_transport`'s `linger`).
 * @param datagram Whether the transport is a datagram one (`detail::resets_input_per_read`): there is no stream to
 *                 end, the descriptor is kept as before.
 * @param reason   The reason the io object ended the connection with. One of the io's own size guards --
 *                 `message_too_large`, `buffer_overflow` -- closes at once: nothing was said that a reset could
 *                 destroy, the peer is the one that sent too much or reads too little, and reading it for the bound
 *                 would serve the flood the guard just refused.
 * @param linger   The object's own bound (`io<>::linger()`), `default_linger` for one that has none (`input<>`,
 *                 `output<>`). Zero or less is "at once", as `set_linger()` documents: a negative one is never read
 *                 as `keep_descriptor`, which would keep the half-closed descriptor in the object.
 * @return `keep_descriptor`, zero ("close at once") or `linger`, positive.
 */
[[nodiscard]] constexpr qb::duration
close_bound(bool datagram, int reason, qb::duration linger) noexcept {
    if (datagram)
        return keep_descriptor;
    if (reason == static_cast<int>(event::disconnect_reason::message_too_large)
        || reason == static_cast<int>(event::disconnect_reason::buffer_overflow))
        return qb::duration::zero();
    return linger > qb::duration::zero() ? linger : qb::duration::zero();
}

/**
 * @brief A transport that carries a TLS session this side can end: `tcp::ssl::socket`, the transport of every TLS
 *        stream.
 * @details TLS has its own end of stream -- the `close_notify` alert -- and a peer that reads the TCP FIN without it
 *          cannot tell a stream its sender ended from one something cut (OpenSSL 3 reports an unexpected EOF). The
 *          transport sends it (`close_notify()`: once, never awaited, never blocking) or ends its session without
 *          a word (`quiet_shutdown()`); WHICH is decided here, where the io object knows whether it wrote
 *          everything it had queued, whoever ended the connection (`end_cut_stream`, `end_transport`; Huly QB-1270).
 */
template <typename Transport>
concept tls_transport = requires(Transport &transport) {
    { transport.close_notify() } -> std::same_as<bool>;
    transport.quiet_shutdown();
};

/**
 * @brief End without a word the TLS stream of a connection that does not end as a whole one; nothing on any other
 *        transport.
 * @details A connection that ends with output still queued, whoever ended it: the output is dropped, so the stream
 *          is cut short and a `close_notify` would tell the peer it is whole. Every other stream is left to say it:
 *          through `end_transport` when the descriptor goes to the loop, from `ssl::socket::disconnect()` when the
 *          transport closes with its object -- a session's destructor (a linger of zero, a size guard), a connection
 *          the peer ended, where the alert answers the peer's own --, and not a second time after a
 *          lingering `close_after_deliver()`. `io<>::dispose()` and `output<>::dispose()` judge the stream BEFORE
 *          `on(event::disconnected&&)` runs, with what is queued then -- a hook that closes the transport itself, or
 *          empties the queue, changes nothing -- and a session's once more after it, with what the hook queued; an
 *          `input<>` has no queue and is judged once. Inside the hook a stream so ended takes no more output.
 * @param complete      Whether the object wrote everything it had queued: its `pendingWrite()` is zero.
 */
template <typename Transport>
constexpr void
end_cut_stream([[maybe_unused]] Transport &transport, [[maybe_unused]] bool complete) noexcept {
    if constexpr (tls_transport<Transport>) {
        if (!complete)
            transport.quiet_shutdown();
    }
}

/**
 * @brief Mark as cut the TLS stream of a transport that leaves its object with output still queued; nothing on any
 *        other transport.
 * @details `io_handler::extractSession()`: the session's queue dies with the session and the transport goes on under
 *          another owner. The octets are missing from the middle of the stream: its end must not say it is whole
 *          (`ssl::socket::mark_cut()`), and the stream must go on meanwhile -- which is why it is not ended here.
 */
template <typename Transport>
constexpr void
mark_cut_stream([[maybe_unused]] Transport &transport, [[maybe_unused]] bool output_queued) noexcept {
    if constexpr (requires { transport.mark_cut(); }) {
        if (output_queued)
            transport.mark_cut();
    }
}

/**
 * @brief Shut the sending side of a connection whose output is delivered: a TLS transport tells its peer first.
 * @details The half-close of a lingering `close_after_deliver()` (Huly QB-1089): the alert follows the delivered
 *          bytes and precedes the FIN, and the session then reads its peer through TLS as before -- the peer's own
 *          `close_notify` ends the linger like its EOF. The session is in a dispatch: its socket is non-blocking.
 * @return Whether the socket took the shutdown.
 */
template <typename Transport>
[[nodiscard]] bool
shutdown_send([[maybe_unused]] Transport &transport, std::remove_cv_t<decltype(qb::io::invalid_socket)> handle) noexcept {
    if constexpr (tls_transport<Transport>)
        static_cast<void>(transport.close_notify());
    return ::shutdown(handle, QB_SD_SEND) == 0;
}

/**
 * @brief End the transport of an io object's connection, once its watcher is stopped.
 * @details A standalone object -- a client, an acceptor: no server owns it -- keeps its transport for as long
 *          as it lives, and before 3.3 its `dispose()` stopped the watcher and left the transport as it was: after a
 *          `disconnect()`, a framing error or the peer's EOF the connection stayed open until the object
 *          connected again or was destroyed. The peer of a `disconnect()` never saw EOF, so a server kept the
 *          connection and the work it routed for it alive, and a client whose peer hung up stayed in CLOSE_WAIT
 *          (Huly QB-1060; qbm-http's HTTP/2 client met it as QB-1021). Every standalone `dispose()` calls this
 *          after `on(event::disconnected&&)`, which still sees the transport as the connection left it (a TLS
 *          stream that is cut is already ended there, and so is one a fatal error ended: it takes no output), and
 *          before `on(event::dispose&&)`, the one hook that may free the object; the watcher is stopped first,
 *          since libev must not watch a descriptor closed under it. How depends on who ended the connection:
 *
 *          - The peer or the socket (`ended_by_peer`): its EOF, a read or a write error. Nothing of ours can be
 *            lost -- the peer sends nothing more and what it sent was read -- so the transport is closed
 *            outright: `disconnect()` when it has one, as `stream::close()` does (the TLS socket clears its
 *            connected state there), then `close()`, which releases the descriptor. A TLS socket's
 *            `disconnect()` answers the peer's `close_notify` with this side's own when the peer ended the session
 *            with one and nothing queued is dropped (`complete`); no `close_notify` follows the peer's bare EOF or a
 *            fatal error (OpenSSL holds no session to end, and wrote the fatal alert the error called for itself),
 *            and nothing at all a reset (the socket recorded its end when it met it).
 *          - This object: `disconnect()`, a framing error, a size guard, the end of `close_after_deliver()`.
 *            Closing here would reset a peer whose bytes are still unread: its next send fails, and on Windows it
 *            also drops what it received and has not read yet -- the tail of the very reply
 *            `close_after_deliver()` promised. Keeping the half-closed descriptor in the object, as 3.3 did, left
 *            a peer that kept writing blocked for ever and only postponed the reset to the object's destruction
 *            or its next connection (Huly QB-1270). So the descriptor leaves the object: a connected stream
 *            socket goes to the thread's loop, which shuts its sending side down -- the peer reads EOF once
 *            everything queued has left -- reads and drops what the peer still sends, and closes it on the peer's
 *            EOF, a socket error, `linger`, or the closer's byte budget (`detail::close_after_peer`). The
 *            transport is closed for the object from here on (`is_open()` is false, as after a peer-ended
 *            connection), which is free, destroyed or connected again at once. A `linger` of zero closes at once.
 *            A TLS transport tells its peer first, while it still owns the descriptor: one `close_notify`, when
 *            the object wrote everything it had queued (`complete`) -- whatever the bound, a `linger` of zero
 *            included: the bound is how long the peer is waited for, not whether the stream is ended properly.
 *            The alert is never waited for, and what the peer answers is dropped by the loop as bytes.
 *            A listening socket has no sending side and is not handed over: an acceptor keeps its port until its
 *            destruction. A datagram transport (`keep_descriptor`) and a transport that cannot give its
 *            descriptor away are shut down for sending and kept, as before.
 *
 *          `dispose()` of a server session calls it too, for a session that ended its own connection with nothing
 *          left to write (`io<>::dispose()`): the server releases the session at once and the loop closes the
 *          descriptor after the peer, instead of the session's destructor closing it with input unread.
 *
 *          Both are no-ops on a closed socket, so a hook that closes the transport again does nothing. A
 *          transport whose handle is not a socket is not shut down; one without `close()` is not closed.
 *          Out of line and cold, like the hooks around it.
 * @param transport     The object's transport (`Derived.transport()`).
 * @param ended_by_peer Whether the peer's EOF or a socket error ended the connection, rather than this object.
 * @param complete      Whether the object wrote everything it had queued -- its
 *                      `pendingWrite()` was zero before `on(event::disconnected&&)` ran (a hook that clears the
 *                      queue drops output all the same) and is zero after it. What a TLS transport's `close_notify`
 *                      asserts, and read by one alone: an object that disconnects with output still queued drops
 *                      it, that stream is cut, and the alert would tell the peer a truncated message is whole -- the
 *                      ambiguity the alert exists to remove, for a body the end of the connection delimits.
 * @param linger        For a connection this object ended: how long the loop waits for the peer before it closes
 *                      (`detail::close_bound`); zero closes at once, `keep_descriptor` keeps the descriptor.
 */
template <typename Transport>
QB_NOINLINE QB_COLD void
end_transport(Transport &transport, bool ended_by_peer, [[maybe_unused]] bool complete, qb::duration linger) noexcept {
    end_cut_stream(transport, complete); // output dropped since the stream was judged (a hook that queued more)
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
        if (handle == qb::io::invalid_socket)
            return;
        // Not a listening socket (`accept()`), and not a transport that cannot give its descriptor away.
        if constexpr (requires { transport.release_handle(); } && !requires { transport.accept(); }) {
            if (linger < qb::duration::zero()) {
                static_cast<void>(::shutdown(handle, QB_SD_SEND)); // kept: the FIN leaves after what is queued
                return;
            }
            // A TLS stream this object wrote whole is ended for the peer while the descriptor is still the
            // transport's: one alert, never awaited -- on a socket made non-blocking first, as the closer makes it (a
            // transport never started may still block, and a full send buffer must not hold the close). A stream it
            // cut is not: nothing is written once the descriptor is released.
            if constexpr (tls_transport<Transport>) {
                if (complete) {
                    static_cast<void>(qb::io::socket::set_nonblocking(handle, true));
                    static_cast<void>(transport.close_notify());
                }
            }
            // The transport lets go of the descriptor first (a TLS socket detaches its SSL object from it there),
            // then is closed as the peer-ended path closes it: it finds no descriptor, and the object keeps nothing
            // that names a number the loop will close and the system hand out again.
            const handle_type released = transport.release_handle();
            if constexpr (qb::has_disconnect<Transport>)
                static_cast<void>(transport.disconnect());
            if constexpr (requires { transport.close(); })
                transport.close();
            close_after_peer(released, linger);
        } else {
            static_cast<void>(::shutdown(handle, QB_SD_SEND)); // the FIN leaves after what is queued
        }
    }
}

} // namespace qb::io::async::detail

#endif // QB_IO_ASYNC_TEARDOWN_H
