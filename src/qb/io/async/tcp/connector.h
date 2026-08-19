/**
 * @file qb/io/async/tcp/connector.h
 * @brief Asynchronous TCP connection establishment utilities
 *
 * This file provides utilities for establishing asynchronous TCP connections.
 * It defines the connector class template which handles the async connection
 * process and a connect function for initiating asynchronous connections.
 *
 * C++20 Coroutine Support:
 * ========================
 *
 * This file also provides C++20 coroutine awaiters for async TCP connections,
 * enabling `co_await` style programming:
 *
 * @code
 * #include <qb/io/async/tcp/connector.h>
 *
 * qb::io::async::task<void> my_connection() {
 *     using namespace std::chrono_literals;
 *
 *     auto socket = co_await qb::io::async::tcp::connect(
 *         qb::io::uri{"tcp://localhost:6379"},
 *         5s
 *     );
 *
 *     if (!socket) {
 *         // Connection failed
 *         co_return;
 *     }
 *
 *     // Use connected socket
 *     // ...
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
 * @ingroup TCP
 */

#ifndef QB_IO_ASYNC_TCP_CONNECTOR_H
#define QB_IO_ASYNC_TCP_CONNECTOR_H

#include <atomic>
#include <chrono>
#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>

// <coroutine> belongs here, not beside the coroutine section further down: an #include
// processed inside `namespace qb::io::async::tcp` would declare
// `qb::io::async::tcp::std` rather than `::std`. It is harmless today only because
// something earlier already pulled these in -- an ordering accident, not a guarantee.
// See qbm/pgsql/src/qbm/pgsql/commands.h for the same defect caught live.
// qb/scripts/check-namespace-scoped-includes.py enforces this.
#ifdef __cpp_impl_coroutine
#include <coroutine>
#endif

#include <qb/io.h>
#include <qb/io/system/sys__socket.h>
#include "../../uri.h"
#include "../../transport/tcp.h"
#include "../event/io.h"
#include "../io.h"
#include "../listener.h"

namespace qb::io::async::tcp {

/**
 * @brief Outcome of one step of a STARTTLS / opportunistic-TLS negotiation.
 *
 * A negotiator drives a plaintext exchange on the freshly-connected socket and,
 * on each readiness event, returns what it needs next. The connector reuses its
 * existing event-loop machinery (watcher, deadline, lifetime) to honor it:
 *   - want_write   : arm EV_WRITE and call the negotiator again when writable.
 *   - want_read    : arm EV_READ and call the negotiator again when readable.
 *   - upgrade      : negotiation agreed on TLS — set up client SSL on the (already
 *                    connected) fd and drive the TLS handshake to completion.
 *   - keep_plaintext: the peer declined TLS; deliver the socket as-is (the caller
 *                    is responsible for continuing in cleartext).
 *   - fail         : abort the connection.
 */
enum class starttls_action { want_write, want_read, upgrade, keep_plaintext, fail };

/**
 * @brief Default "no negotiation" policy — the connector performs a plain or
 *        direct-TLS connect exactly as before. `enabled == false` makes every
 *        STARTTLS branch `if constexpr`-discarded, so the classic path is unchanged.
 */
struct no_negotiation {
    static constexpr bool enabled = false;
};

/**
 * @brief Concept for a STARTTLS negotiator usable with the connector.
 *
 * A negotiator is a small, default-constructible state machine. After the TCP
 * connect completes, the connector calls `advance(plaintext_socket, revents)` on
 * each I/O readiness event; the negotiator performs its own non-blocking plaintext
 * I/O (it owns the wire format — PostgreSQL SSLRequest, SMTP/IMAP STARTTLS lines,
 * …) and returns a @ref starttls_action. `enabled` must be `true`.
 */
template <typename N>
concept StarttlsNegotiator = requires(N n, qb::io::tcp::socket &s, int revents) {
    { N::enabled } -> std::convertible_to<bool>;
    { n.advance(s, revents) } -> std::same_as<starttls_action>;
};

/**
 * @class connector
 * @brief Handles asynchronous TCP connection establishment
 *
 * Manages non-blocking `n_connect`, completion on `EV_WRITE`, and an optional
 * wall-clock deadline via `async::callback`. The instance is kept alive with
 * `std::shared_ptr` and `self_hold_` until exactly one completion is delivered to
 * the user callback (so the object is not destroyed while libev still references
 * it).
 *
 * @note **Completion ordering:** `on(event::io const &)` always unregisters the I/O
 *       watcher using `event._interface` *before* checking whether another path
 *       (e.g. deadline) already completed. Returning early *without* unregistering
 *       caused invalid-fd regressions on kqueue/epoll.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 */
template <typename Socket_, typename Func_, typename Negotiator_ = no_negotiation>
class connector : public std::enable_shared_from_this<connector<Socket_, Func_, Negotiator_>> {
    Func_   func_;   /**< Callback function to call when connection completes */
    Socket_ socket_; /**< Socket for the connection */
    uri     remote_; /**< URI of the remote endpoint */
    /** Absolute libev time `ev_time() + timeout` when `timeout > 0`; else `0` (no deadline). */
    const double deadline_;
    /** When false, disable TLS peer verification on a secure socket (opt-out). */
    bool verify_peer_{true};

    bool                       completed_{false};
    bool                       deadline_armed_{false};
    IRegisteredKernelEvent    *io_iface_{nullptr};
    std::shared_ptr<connector> self_hold_;

    // STARTTLS / opportunistic-TLS state (only used when Negotiator_::enabled).
    enum class sphase { connecting, negotiating, handshaking };
    sphase      sphase_{sphase::connecting};
    Negotiator_ neg_{};

    /**
     * @brief Marks this connect attempt as finished for callback purposes.
     * @return true if this call is the first completion; false if already completed.
     */
    [[nodiscard]] bool
    mark_completed_once() noexcept {
        if (completed_)
            return false;
        completed_ = true;
        return true;
    }

    /**
     * @brief Invokes the user callback and releases the self-retention ref.
     * @private
     */
    void
    deliver(Socket_ &&s) {
        auto guard = std::exchange(self_hold_, nullptr);
        func_(std::move(s));
    }

    /**
     * @brief Listener-teardown reclaim hook for the in-flight connect's io watcher.
     * @details Registered as the watcher's `_destroy_owner` (see arm_io). When the event loop
     *          is torn down (`listener::clear()` / `~listener`) while a connect is still in
     *          flight, clear() invokes this to break the self-hold that would otherwise orphan
     *          the connector, its captured completion callback, and the half-open socket fd.
     *          The watcher is already detached by clear(); unregisterEvent frees its wrapper via
     *          the `_detached_by_clear` path, and dropping the self-ref reclaims the connector.
     * @private
     */
    static void
    on_listener_teardown(void *p) noexcept {
        auto *self = static_cast<connector *>(p);
        if (self->io_iface_ != nullptr) {
            listener::current.unregisterEvent(self->io_iface_);
            self->io_iface_ = nullptr; // null before the connector is destroyed below
        }
        self->self_hold_.reset(); // last strong ref -> reclaim the connector + its callback
    }

    void
    deliver_failure() {
        socket_.disconnect();
        if (mark_completed_once())
            deliver(Socket_{});
    }

    // Deliver an *immediate* (synchronous) connect failure on the next event-loop
    // turn instead of inline. n_connect() can fail synchronously — notably on
    // Windows, where a non-blocking connect to a closed loopback port reports the
    // refusal right away rather than deferring it via WSAEWOULDBLOCK — and
    // delivering the failure inline re-enters the caller (e.g. a client that pushes
    // a request from inside its own disconnect/failure handler, which then re-enters
    // that handler and drops work queued during the current pass). `async::defer()`
    // posts it to the tail of the current loop turn, so the completion callback is
    // never invoked re-entrantly from run(), exactly matching the POSIX path where a
    // non-blocking connect goes EINPROGRESS and the result is reported from the loop
    // (EV_WRITE / deadline).
    void
    deliver_failure_deferred() {
        // Bind the connector's lifetime to the deferred callback via a STRONG capture — NOT
        // self_hold_ + a weak capture. This path arms no io watcher, so it installs no
        // on_listener_teardown reclaim hook (that lives in arm_io). A self_hold_ self-cycle + weak
        // capture would LEAK if the deferred callback is dropped by listener::clear()/~listener before
        // it runs: the callback never fires, so the cycle is never broken. With a strong capture the
        // connector lives exactly as long as the deferred callback — firing delivers then releases it;
        // teardown clearing the defer queue (and its captured shared_ptr) also releases it. Either way
        // the connector + its captured completion callback are reclaimed; no leak.
        auto self = this->shared_from_this();
        qb::io::async::defer([self = std::move(self)]() { self->deliver_failure(); });
    }

    enum class finalize_result { done, pending, failed };

    [[nodiscard]] bool
    arm_io(int events) {
        if (!socket_.is_open() || socket_.native_handle() < 0)
            return false;

        if (!self_hold_)
            self_hold_ = this->shared_from_this();

        if (io_iface_)
            return true;

        auto &io_ev = listener::current.registerEvent<event::io>(*this, socket_.native_handle(), events);
        io_iface_   = io_ev._interface;
        // Make the watcher loop-owned: if the loop is torn down before this connect completes,
        // clear() reclaims the connector through on_listener_teardown instead of leaking the
        // self-held connector + callback + half-open fd.
        io_iface_->set_owner(this, &connector::on_listener_teardown);
        io_ev.start();
        return true;
    }

    void
    arm_deadline() {
        if (deadline_ <= 0. || deadline_armed_)
            return;
        deadline_armed_                 = true;
        const double             remain = deadline_ - ev_time();
        std::weak_ptr<connector> w      = this->shared_from_this();
        qb::io::async::callback(
            [w]() {
                if (auto self = w.lock())
                    self->on_deadline();
            },
            qb::detail::from_ev_seconds(remain > 0. ? remain : 0.));
    }

    [[nodiscard]] finalize_result
    finalize_transport_connect() noexcept {
        if constexpr (requires(Socket_ &s) {
                          { s.handshake_status() } -> std::same_as<int>;
                      }) {
            const auto status = socket_.handshake_status();
            if (status > 0)
                return finalize_result::done;
            if (status == 0)
                return finalize_result::pending;
            return finalize_result::failed;
        } else if constexpr (std::is_same_v<decltype(std::declval<Socket_ &>().connected()), int>) {
            return socket_.connected() == 0 ? finalize_result::done : finalize_result::failed;
        } else {
            socket_.connected();
            return finalize_result::done;
        }
    }

public:
    /**
     * @brief Constructs a connector and stores parameters (does not connect yet).
     * @param func Callback invoked exactly once with the connected socket or an empty socket
     * @param remote Remote URI
     * @param timeout_sec Connection deadline in seconds from construction (`ev_time()`);
     *                    `0` means no deadline timer (wait indefinitely for writability).
     */
    connector(Func_ &&func, uri remote, double timeout_sec, bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer) {}

    /**
     * @brief Constructs a connector with an existing socket (does not connect yet).
     * @param func Callback invoked exactly once on completion
     * @param existing Socket to use (moved from)
     * @param remote Remote URI
     * @param timeout_sec Same semantics as the other constructor
     * @param verify_peer When false, disables TLS peer verification (secure sockets only).
     */
    connector(Func_ &&func, Socket_ &&existing, uri remote, double timeout_sec, bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , socket_(std::move(existing))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer) {}

    /**
     * @brief Runs `n_connect` and either completes immediately or registers `EV_WRITE`
     *        (and optionally a deadline callback).
     */
    void
    run() {
        if constexpr (Negotiator_::enabled) {
            run_starttls();
            return;
        }
        QB_LOG_DEBUG("Started async connect to " << remote_.source());
        // Apply the TLS verification policy before the (non-blocking) connect so
        // it is in effect when the handshake starts. No-op for plain sockets.
        if constexpr (requires { socket_.set_insecure(); }) {
            if (!verify_peer_)
                socket_.set_insecure();
        }
        auto ret = socket_.n_connect(remote_);
        if (!ret) {
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    if (!mark_completed_once())
                        return;
                    QB_LOG_DEBUG("Connected directly to " << remote_.source());
                    deliver(std::move(socket_));
                    break;
                case finalize_result::pending:
                    if (arm_io(EV_READ | EV_WRITE)) {
                        arm_deadline();
                    } else {
                        deliver_failure_deferred();
                    }
                    break;
                case finalize_result::failed:
                    QB_LOG_DEBUG("Failed to finalize direct connect to " << remote_.source());
                    deliver_failure_deferred();
            }
            return;
        }
        if (socket_no_error(qb::io::socket::get_last_errno()) && arm_io(EV_WRITE)) {
            arm_deadline();
            return;
        }

        QB_LOG_DEBUG("Failed to connect to " << remote_.source() << " err=" << qb::io::socket::get_last_errno());
        deliver_failure_deferred();
    }

    /**
     * @brief I/O event handler when the socket becomes writable (connect completion).
     * @param event The I/O event (must unregister `event._interface` here, always).
     */
    void
    on(event::io const &event) {
        if constexpr (Negotiator_::enabled) {
            on_starttls(event);
            return;
        }
        int err = 0;
        if (!(event._revents & (EV_READ | EV_WRITE)) || socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            socket_.disconnect();
            err = 1;
        }

        if (!err || err == EISCONN) {
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    listener::current.unregisterEvent(event._interface);
                    io_iface_ = nullptr;
                    if (!mark_completed_once())
                        return;
                    QB_LOG_DEBUG("Connected async to " << remote_.source());
                    deliver(std::move(socket_));
                    return;
                case finalize_result::pending:
                    static_cast<event::io &>(const_cast<event::io &>(event)).set(EV_READ | EV_WRITE);
                    return;
                case finalize_result::failed:
                    break;
            }

            socket_.disconnect();
        }

        listener::current.unregisterEvent(event._interface);
        io_iface_ = nullptr;
        if (!mark_completed_once())
            return;
        QB_LOG_DEBUG("Failed to connect to " << remote_.source() << " err=" << err);
        deliver(Socket_{});
    }

    /**
     * @brief Deadline handler: unregister the write watcher if still registered,
     *        close the socket, and complete with failure if still the first completion.
     */
    void
    on_deadline() {
        if (io_iface_) {
            listener::current.unregisterEvent(io_iface_);
            io_iface_ = nullptr;
        }
        socket_.disconnect();

        if (!mark_completed_once())
            return;

        QB_LOG_DEBUG("Async connect deadline for " << remote_.source());
        deliver(Socket_{});
    }

    // =========================================================================
    // STARTTLS / opportunistic-TLS state machine
    //
    // Only instantiated when Negotiator_::enabled. Reuses the connector's existing
    // watcher (arm_io), deadline (arm_deadline/on_deadline), self-hold lifetime,
    // and TLS handshake pump (finalize_transport_connect). The flow is:
    //   tcp connect (cleartext) -> negotiate (negotiator owns the wire format)
    //     -> on "upgrade": init_client() + drive the handshake to completion
    //     -> on "keep_plaintext": deliver the cleartext socket
    //     -> on "fail": abort.
    // =========================================================================

    /// SNI / verification hostname for the TLS upgrade (the remote URI host).
    std::string
    starttls_host() const {
        return std::string(remote_.host());
    }

    /// Unregister the watcher and deliver exactly once (success -> the socket,
    /// failure -> an empty/closed socket). Mirrors the classic on()/on_deadline().
    void
    finish_starttls(event::io const &event, bool ok) {
        listener::current.unregisterEvent(event._interface);
        io_iface_ = nullptr;
        if (!ok)
            socket_.disconnect();
        if (!mark_completed_once())
            return;
        deliver(ok ? std::move(socket_) : Socket_{});
    }

    void
    run_starttls() {
        QB_LOG_DEBUG("Started async STARTTLS connect to " << remote_.source());
        if (!verify_peer_)
            socket_.set_insecure();
        // Connect the underlying TCP layer ONLY. ssl::socket::n_connect() would set
        // up the SSL client state immediately; that must wait until after the
        // cleartext negotiation agrees to upgrade, so go through the tcp base.
        auto &raw = static_cast<qb::io::tcp::socket &>(socket_);
        auto  ret = raw.n_connect(remote_);
        if (ret && !socket_no_error(qb::io::socket::get_last_errno())) {
            deliver_failure_deferred();
            return;
        }
        sphase_ = ret ? sphase::connecting : sphase::negotiating;
        if (arm_io(EV_WRITE))
            arm_deadline();
        else
            deliver_failure_deferred();
    }

    void
    on_starttls(event::io const &event) {
        auto &mutable_event = const_cast<event::io &>(event);
        int   err           = 0;
        if (!(event._revents & (EV_READ | EV_WRITE)) || socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            finish_starttls(event, false);
            return;
        }
        if (err && err != EISCONN) {
            finish_starttls(event, false);
            return;
        }

        if (sphase_ == sphase::connecting)
            sphase_ = sphase::negotiating; // TCP connect just completed

        if (sphase_ == sphase::negotiating) {
            switch (neg_.advance(static_cast<qb::io::tcp::socket &>(socket_), event._revents)) {
                case starttls_action::want_write:
                    mutable_event.set(EV_WRITE);
                    return;
                case starttls_action::want_read:
                    mutable_event.set(EV_READ);
                    return;
                case starttls_action::keep_plaintext:
                    finish_starttls(event, true); // deliver the cleartext socket as-is
                    return;
                case starttls_action::fail:
                    finish_starttls(event, false);
                    return;
                case starttls_action::upgrade:
                    if (socket_.init_client(starttls_host()) != 0) {
                        finish_starttls(event, false);
                        return;
                    }
                    sphase_ = sphase::handshaking;
                    break; // fall through and pump the handshake immediately
            }
        }

        if (sphase_ == sphase::handshaking) {
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    finish_starttls(event, true);
                    return;
                case finalize_result::pending:
                    mutable_event.set(EV_READ | EV_WRITE);
                    return;
                case finalize_result::failed:
                    finish_starttls(event, false);
                    return;
            }
        }
    }
};

/**
 * @brief Initiates an asynchronous TCP connection
 *
 * Allocates a `std::shared_ptr<connector>` and calls `run()`. The connector stays
 * alive until the user callback runs (including across `n_connect` in progress).
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (`0` = no deadline, same as before)
 * @param verify_peer For secure transports, whether to verify the server
 *                    certificate chain + hostname (default `true`, secure).
 *                    Pass `false` only for trusted/self-signed channels.
 */
// `requires std::invocable<...>` is LOAD-BEARING, not decoration (it mirrors the guard the
// starttls_connect overloads already carry). `Socket_` is not deducible, so the moment a caller
// writes the documented coroutine form WITH an explicit transport and a timeout —
// `co_await connect<qb::io::transport::stcp>(uri, 5s)` — this overload also becomes viable:
// `Func_` deduces to `std::chrono::seconds` (an EXACT match), beating the coroutine overload's
// `qb::duration` parameter (a chrono conversion). It then wins overload resolution and the build
// dies deep inside `connector<transport::stcp, std::chrono::seconds>`. Constraining `Func_` to
// things actually callable with a socket removes this overload from that contest, so the
// coroutine factory is selected as documented. No test caught it because the tests only ever call
// `connect(uri, timeout)` without an explicit template argument (where `Socket_` is non-deducible
// and this overload is already excluded).
template <typename Socket_, typename Func_>
requires std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
connect(uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), remote, qb::detail::to_ev_seconds(timeout), verify_peer);
    QB_LOG_DEBUG("Connector: Initializing for " << remote.source());
    op->run();
}

/**
 * @brief Initiates an asynchronous TCP connection using an existing socket
 *
 * Same as `connect(uri, func, timeout)` but moves an existing `Socket_` into the
 * connector before `n_connect`.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param existing_socket Existing socket (moved from)
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (`0` = no deadline)
 */
// Same constraint as the uri-first overload above — see its note. Keeping BOTH callback
// overloads constrained is what stops the family from drifting apart again.
template <typename Socket_, typename Func_>
requires std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
connect(Socket_ &&existing_socket, uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), std::move(existing_socket), remote,
                                                          qb::detail::to_ev_seconds(timeout), verify_peer);
    QB_LOG_DEBUG("Connector: Initializing with existing socket for " << remote.source());
    op->run();
}

/**
 * @brief Initiate an asynchronous opportunistic-TLS (STARTTLS) connection.
 *
 * Connects the TCP layer in cleartext, runs @p Negotiator_ 's plaintext negotiation
 * (it owns the wire format — PostgreSQL SSLRequest, SMTP/IMAP `STARTTLS`, …), and,
 * if the peer agrees, completes a TLS client handshake — all asynchronously on the
 * event loop, reusing the same watcher/deadline/lifetime machinery as `connect()`.
 * The callback receives a ready @p Socket_ (secure when the upgrade happened) or an
 * empty/closed socket on failure, exactly like `connect()`.
 *
 * @tparam Socket_ The (secure) socket type to deliver, e.g. `qb::io::tcp::ssl::socket`.
 * @tparam Negotiator_ A @ref StarttlsNegotiator policy.
 * @tparam Func_ Callback type, invoked once with `Socket_&&`.
 */
template <typename Socket_, typename Negotiator_, typename Func_>
requires StarttlsNegotiator<Negotiator_> && std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
starttls_connect(uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_, Negotiator_>>(std::forward<Func_>(func), remote, qb::detail::to_ev_seconds(timeout),
                                                                       verify_peer);
    QB_LOG_DEBUG("Connector: Initializing STARTTLS for " << remote.source());
    op->run();
}

/**
 * @brief STARTTLS connect with a caller-supplied socket carrying its own TLS policy.
 * @details Mirrors `connect(existing, remote, func, ...)` for the opportunistic-TLS path: pass a secure
 *          socket already built from a `qb::io::ssl::Context` (custom CA via `trust()`, client certificate
 *          via `identity()`, verify mode, ALPN, …). There is no `verify_peer` bool — the socket's Context
 *          governs verification, so the connector never forces `set_insecure()`. This is the escape from the
 *          bool-only limitation of the other overload: custom-CA / client-cert / mTLS over STARTTLS
 *          (PostgreSQL `SSLRequest`, SMTP/IMAP `STARTTLS`, …). The socket fails CLOSED on a broken Context.
 * @tparam Socket_ The (secure) socket type to deliver, e.g. `qb::io::tcp::ssl::socket`.
 * @tparam Negotiator_ A @ref StarttlsNegotiator policy.
 */
template <typename Socket_, typename Negotiator_, typename Func_>
requires StarttlsNegotiator<Negotiator_> && std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
starttls_connect(Socket_ &&existing, uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero()) {
    auto op = std::make_shared<connector<Socket_, Func_, Negotiator_>>(std::forward<Func_>(func), std::move(existing), remote,
                                                                       qb::detail::to_ev_seconds(timeout), /*verify_peer*/ true);
    QB_LOG_DEBUG("Connector: Initializing STARTTLS (Context socket) for " << remote.source());
    op->run();
}

// =============================================================================
// C++20 Coroutine Support
// =============================================================================

#ifdef __cpp_impl_coroutine
// Coroutines are available (C++20/23).
// <coroutine>, <optional> and <chrono> are included at the top of this file, outside
// `namespace qb::io::async::tcp` -- see the note there.

/**
 * @defgroup CoroutineTCP Coroutine TCP Connectors
 * @brief C++20 coroutine awaiters for TCP connections
 *
 * These classes enable `co_await` style programming for TCP connections,
 * wrapping the callback-based connector with a modern coroutine interface.
 *
 * @code
 * auto socket = co_await qb::io::async::tcp::connect(
 *     uri{"tcp://localhost:6379"}, 5s
 * );
 * if (socket) { // use socket
 *     // ...
 * }
 * @endcode
 */

/**
 * @brief Coroutine awaiter for TCP connection establishment
 * @ingroup CoroutineTCP
 * @tparam Socket_ The socket type
 *
 * This awaiter wraps the callback-based tcp::connect with a C++20 coroutine
 * interface. It suspends the coroutine until connection completes and resumes
 * with std::optional<Socket_>.
 */
template <typename Socket_>
class connect_awaiter {
    struct state_t {
        std::optional<Socket_>               result;
        std::coroutine_handle<>              handle{};
        ::qb::io::async::CoroutineScheduler *scheduler{nullptr};
        bool                                 ready{false};
        bool                                 active{true};
    };

    uri                      _remote;
    qb::duration             _timeout;
    bool                     _verify_peer{true};
    std::shared_ptr<state_t> _state{std::make_shared<state_t>()};

public:
    explicit connect_awaiter(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true)
        : _remote(std::move(remote))
        , _timeout(timeout)
        , _verify_peer(verify_peer) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->ready;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->handle    = h;
        _state->scheduler = ::qb::io::async::CoroutineScheduler::current_ptr();
        if (!_state->scheduler)
            _state->scheduler = &::qb::io::async::CoroutineScheduler::current();

        auto state = _state;
        ::qb::io::async::tcp::connect<Socket_>(
            _remote,
            [state](Socket_ &&socket) {
                if (!state->active)
                    return;
                if (socket.is_open()) {
                    state->result = std::move(socket);
                }
                state->ready = true;
                // Resolve the scheduler NOW, not at suspend time: the cached one may be the
                // thread-local fallback this awaiter built when nothing was bound yet, which
                // `listener::run()` never pumps. See the long note on
                // `awaiter_base::on_event_ready` (qb/io/async/coroutine/awaiter.h) — this
                // callback runs on the loop thread, so the current scheduler is the pumped one.
                if (auto *target = ::qb::io::async::CoroutineScheduler::current_ptr() ? ::qb::io::async::CoroutineScheduler::current_ptr()
                                                                                      : state->scheduler;
                    target && state->handle) {
                    target->schedule_resume(state->handle);
                }
            },
            _timeout, _verify_peer);
    }

    [[nodiscard]] std::optional<Socket_>
    await_resume() {
        _state->active = false;
        _state->handle = {};
        return std::move(_state->result);
    }

    ~connect_awaiter() {
        _state->active = false;
        _state->handle = {};
    }
};

/**
 * @brief Factory function for TCP connection awaiter
 * @ingroup CoroutineTCP
 * @tparam Transport The transport type (default: transport::tcp)
 * @param remote The remote endpoint URI
 * @param timeout Connection timeout (default: 0ms = no timeout)
 * @return connect_awaiter with appropriate socket type
 */
template <typename Transport = qb::io::transport::tcp>
[[nodiscard]] auto
connect(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    using socket_type = typename Transport::transport_io_type;
    return connect_awaiter<socket_type>{std::move(remote), timeout, verify_peer};
}

/**
 * @brief Awaiter for connecting with existing socket
 * @ingroup CoroutineTCP
 * @tparam Socket_ The socket type
 */
template <typename Socket_>
class connect_with_socket_awaiter {
    struct state_t {
        std::optional<Socket_>               result;
        std::coroutine_handle<>              handle{};
        ::qb::io::async::CoroutineScheduler *scheduler{nullptr};
        bool                                 ready{false};
        bool                                 active{true};
    };

    Socket_                  _socket;
    uri                      _remote;
    qb::duration             _timeout;
    std::shared_ptr<state_t> _state{std::make_shared<state_t>()};

public:
    connect_with_socket_awaiter(Socket_ &&sock, uri remote, qb::duration timeout)
        : _socket(std::move(sock))
        , _remote(std::move(remote))
        , _timeout(timeout) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->ready;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->handle    = h;
        _state->scheduler = ::qb::io::async::CoroutineScheduler::current_ptr();
        if (!_state->scheduler)
            _state->scheduler = &::qb::io::async::CoroutineScheduler::current();

        auto state = _state;
        ::qb::io::async::tcp::connect<Socket_>(
            std::move(_socket), _remote,
            [state](Socket_ &&socket) {
                if (!state->active)
                    return;
                if (socket.is_open()) {
                    state->result = std::move(socket);
                }
                state->ready = true;
                // Resolve the scheduler NOW, not at suspend time: the cached one may be the
                // thread-local fallback this awaiter built when nothing was bound yet, which
                // `listener::run()` never pumps. See the long note on
                // `awaiter_base::on_event_ready` (qb/io/async/coroutine/awaiter.h) — this
                // callback runs on the loop thread, so the current scheduler is the pumped one.
                if (auto *target = ::qb::io::async::CoroutineScheduler::current_ptr() ? ::qb::io::async::CoroutineScheduler::current_ptr()
                                                                                      : state->scheduler;
                    target && state->handle) {
                    target->schedule_resume(state->handle);
                }
            },
            _timeout);
    }

    [[nodiscard]] std::optional<Socket_>
    await_resume() {
        _state->active = false;
        _state->handle = {};
        return std::move(_state->result);
    }

    ~connect_with_socket_awaiter() {
        _state->active = false;
        _state->handle = {};
    }
};

/**
 * @brief Factory function for connecting with existing socket
 * @ingroup CoroutineTCP
 * @tparam Transport The transport type (default: transport::tcp)
 * @param existing_socket Socket to use for the connection (will be moved)
 * @param remote The remote endpoint URI
 * @param timeout Connection timeout (default: 0ms = no timeout)
 */
template <typename Transport = qb::io::transport::tcp>
[[nodiscard]] auto
connect_with_socket(typename Transport::transport_io_type &&existing_socket, uri remote, qb::duration timeout = qb::duration::zero()) {
    using socket_type = typename Transport::transport_io_type;
    return connect_with_socket_awaiter<socket_type>{std::move(existing_socket), std::move(remote), timeout};
}

/**
 * @brief Coroutine awaiter for an opportunistic-TLS (STARTTLS) connection.
 * @ingroup CoroutineTCP
 *
 * The `co_await` counterpart of `starttls_connect()`: suspends until the cleartext
 * connect + negotiation + (optional) TLS handshake complete, then resumes with
 * `std::optional<Socket_>` (empty on failure). Same machinery as @ref connect_awaiter.
 */
template <typename Socket_, typename Negotiator_>
class starttls_connect_awaiter {
    struct state_t {
        std::optional<Socket_>               result;
        std::coroutine_handle<>              handle{};
        ::qb::io::async::CoroutineScheduler *scheduler{nullptr};
        bool                                 ready{false};
        bool                                 active{true};
    };

    uri                      _remote;
    qb::duration             _timeout;
    bool                     _verify_peer{true};
    std::shared_ptr<state_t> _state{std::make_shared<state_t>()};

public:
    explicit starttls_connect_awaiter(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true)
        : _remote(std::move(remote))
        , _timeout(timeout)
        , _verify_peer(verify_peer) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->ready;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->handle    = h;
        _state->scheduler = ::qb::io::async::CoroutineScheduler::current_ptr();
        if (!_state->scheduler)
            _state->scheduler = &::qb::io::async::CoroutineScheduler::current();

        auto state = _state;
        ::qb::io::async::tcp::starttls_connect<Socket_, Negotiator_>(
            _remote,
            [state](Socket_ &&socket) {
                if (!state->active)
                    return;
                if (socket.is_open())
                    state->result = std::move(socket);
                state->ready = true;
                // Resolve the scheduler NOW, not at suspend time: the cached one may be the
                // thread-local fallback this awaiter built when nothing was bound yet, which
                // `listener::run()` never pumps. See the long note on
                // `awaiter_base::on_event_ready` (qb/io/async/coroutine/awaiter.h) — this
                // callback runs on the loop thread, so the current scheduler is the pumped one.
                if (auto *target = ::qb::io::async::CoroutineScheduler::current_ptr() ? ::qb::io::async::CoroutineScheduler::current_ptr()
                                                                                      : state->scheduler;
                    target && state->handle) {
                    target->schedule_resume(state->handle);
                }
            },
            _timeout, _verify_peer);
    }

    [[nodiscard]] std::optional<Socket_>
    await_resume() {
        _state->active = false;
        _state->handle = {};
        return std::move(_state->result);
    }

    ~starttls_connect_awaiter() {
        _state->active = false;
        _state->handle = {};
    }
};

/**
 * @brief Factory for the STARTTLS connection awaiter (parity with `connect()`).
 * @ingroup CoroutineTCP
 * @tparam Transport The secure transport, e.g. `qb::io::transport::stcp`.
 * @tparam Negotiator_ A @ref StarttlsNegotiator policy.
 *
 * Pass all three arguments explicitly (`uri, timeout, verify_peer`) — both template
 * parameters are required, so a 2-argument call would be ambiguous with the callback
 * overload.
 */
template <typename Transport, typename Negotiator_>
requires StarttlsNegotiator<Negotiator_>
[[nodiscard]] auto
starttls_connect(uri remote, qb::duration timeout, bool verify_peer = true) {
    using socket_type = typename Transport::transport_io_type;
    return starttls_connect_awaiter<socket_type, Negotiator_>{std::move(remote), timeout, verify_peer};
}

#endif // __cpp_impl_coroutine

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
