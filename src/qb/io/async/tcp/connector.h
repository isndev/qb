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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

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
 * **Fallback through the addresses (3.3, Huly QB-164).** A host name is resolved to every address of
 * the URI's family, and the addresses are tried in the resolver's order: one whose TCP connect is
 * refused, does not route, fails later (`SO_ERROR`) or does not answer within its share of the deadline
 * (`tcp::connect_attempt_budget`) is closed and the next is tried; the deadline stays the whole
 * connect's. A failure past the TCP connect -- the TLS handshake, a STARTTLS negotiation -- is final: the
 * next address would present the same server. Only a descriptor the connector opens itself is replaced:
 * a socket handed over already open keeps its one attempt.
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

    /** The addresses tried in order (Huly QB-164): given to the endpoint-list constructor, or resolved
     *  by `run()` from the URI in its family. Empty for an AF_UNIX remote, which has its one attempt. */
    std::vector<qb::io::endpoint> endpoints_;
    std::string                   host_;          ///< SNI / verification name for TLS, and the name logged
    bool                          listed_{false}; ///< the endpoints were given, not resolved from `remote_`
    std::size_t                   next_{0};       ///< index of the next endpoint to try
    bool                          own_fd_{true};  ///< the descriptor is the connector's to replace between attempts
    std::uint32_t                 attempt_{0};    ///< generation of the running attempt: a stale attempt timer finds another

    /// A socket the connector can point at one address: `n_connect(endpoint)`. One without it (a custom
    /// socket type) resolves its URI itself and gets the single `n_connect(uri)` attempt it always had.
    static constexpr bool endpoint_capable = requires(Socket_ &s, qb::io::endpoint const &ep) { s.n_connect(ep); };

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

    /// Start one TCP connect: the next endpoint, or the remote URI itself when there is no list (an AF_UNIX
    /// remote, or a socket that resolves its URI itself). Returns `n_connect`'s result; the errno is the
    /// caller's to read.
    int
    connect_step() {
        if constexpr (Negotiator_::enabled) {
            auto &raw = static_cast<qb::io::tcp::socket &>(socket_); // the TLS state waits for the negotiation
            return endpoints_.empty() ? raw.n_connect(remote_) : raw.n_connect(endpoints_[next_ - 1]);
        } else if constexpr (endpoint_capable) {
            if (endpoints_.empty())
                return socket_.n_connect(remote_);
            auto const &ep = endpoints_[next_ - 1];
            if constexpr (requires(Socket_ &s, qb::io::endpoint const &e, std::string const &h) { s.n_connect(e, h); })
                return socket_.n_connect(ep, host_); // TLS: the client state, with SNI and verification for `host_`
            else
                return socket_.n_connect(ep);
        } else {
            return socket_.n_connect(remote_);
        }
    }

    /// Arm the running attempt's share of the deadline, when an address is left after it.
    void
    arm_attempt_deadline() {
        if (deadline_ <= 0. || endpoints_.empty() || !own_fd_)
            return;
        const std::size_t left = endpoints_.size() - next_ + 1; // this attempt and the ones after it
        if (left <= 1)
            return;
        const qb::duration remaining = qb::detail::from_ev_seconds(deadline_ - ev_time());
        const qb::duration budget    = qb::io::tcp::connect_attempt_budget(remaining, left);
        if (budget >= remaining)
            return; // the whole deadline is this attempt's anyway
        std::weak_ptr<connector> w = this->shared_from_this();
        qb::io::async::callback(
            [w, generation = attempt_]() {
                if (auto self = w.lock())
                    self->on_attempt_deadline(generation);
            },
            budget);
    }

    /**
     * @brief Try addresses until one connects, or one is in progress (its completion comes back through
     *        `on()`); deliver the failure when none is left.
     * @details An address refused at once leaves its place to the next. Between two attempts the descriptor
     *          is closed -- a failed connect leaves it unusable -- unless it was handed over open, in which
     *          case that one attempt is all there is.
     */
    void
    start_next() {
        for (;;) {
            if (completed_)
                return;
            if (!endpoints_.empty()) {
                if (next_ >= endpoints_.size())
                    break;
                if (next_ > 0) {
                    if (!own_fd_)
                        break;
                    socket_.disconnect();
                    if constexpr (requires { socket_.close(); })
                        socket_.close(); // a failed connect leaves the descriptor unusable: the next attempt opens its own
                }
                ++next_;
            } else if (attempt_ > 0) {
                break; // AF_UNIX: the one attempt
            }
            ++attempt_;
            const int ret = connect_step();
            const int err = qb::io::socket::get_last_errno();
            if constexpr (Negotiator_::enabled) {
                if (ret && !socket_no_error(err)) {
                    QB_LOG_DEBUG("STARTTLS connect to " << host_ << " refused at once err=" << err);
                    continue;
                }
                sphase_ = ret ? sphase::connecting : sphase::negotiating;
                if (arm_io(EV_WRITE)) {
                    arm_deadline();
                    arm_attempt_deadline();
                    return;
                }
                continue;
            } else {
                if (!ret) {
                    switch (finalize_transport_connect()) {
                        case finalize_result::done:
                            if (!mark_completed_once())
                                return;
                            QB_LOG_DEBUG("Connected directly to " << host_);
                            deliver(std::move(socket_));
                            return;
                        case finalize_result::pending:
                            if (arm_io(EV_READ | EV_WRITE)) {
                                arm_deadline();
                                return; // the TCP part is done: no attempt timer, what follows is final
                            }
                            break;
                        case finalize_result::failed:
                            QB_LOG_DEBUG("Failed to finalize direct connect to " << host_);
                            break;
                    }
                    ++attempt_;
                    deliver_failure_deferred(); // past the TCP connect: final
                    return;
                }
                if (socket_no_error(err) && arm_io(EV_WRITE)) {
                    arm_deadline();
                    arm_attempt_deadline();
                    return;
                }
                QB_LOG_DEBUG("Failed to connect to " << host_ << " err=" << err);
            }
        }
        ++attempt_;
        deliver_failure_deferred();
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
        , verify_peer_(verify_peer)
        , host_(remote_.host()) {}

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
        , verify_peer_(verify_peer)
        , host_(remote_.host()) {}

    /**
     * @brief Constructs a connector over a list of endpoints, tried in order (since 3.3; does not connect yet).
     * @param func Callback invoked exactly once on completion
     * @param endpoints Addresses to try, in order; an empty list completes with a failure
     * @param host Name for TLS (SNI and certificate verification); ignored by a plain socket
     * @param timeout_sec Same semantics as the other constructors: the whole connect's deadline
     * @param verify_peer When false, disables TLS peer verification (secure sockets only).
     */
    connector(Func_ &&func, std::vector<qb::io::endpoint> endpoints, std::string host, double timeout_sec, bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer)
        , endpoints_(std::move(endpoints))
        , host_(std::move(host))
        , listed_(true) {
        static_assert(endpoint_capable, "connecting over an endpoint list needs a socket with n_connect(endpoint)");
    }

    /**
     * @brief The endpoint-list constructor with an existing socket (since 3.3; does not connect yet).
     * @details The socket is moved in -- one built from a `qb::io::ssl::Context` carries its TLS policy. A
     *          socket that is not open yet falls back through the list; one already open gets the first
     *          address only (see the class note).
     */
    connector(Func_ &&func, Socket_ &&existing, std::vector<qb::io::endpoint> endpoints, std::string host, double timeout_sec,
              bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , socket_(std::move(existing))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer)
        , endpoints_(std::move(endpoints))
        , host_(std::move(host))
        , listed_(true) {
        static_assert(endpoint_capable, "connecting over an endpoint list needs a socket with n_connect(endpoint)");
    }

    /**
     * @brief Resolves the remote when it is a host name, then starts the first attempt.
     * @details Each attempt runs `n_connect` and either completes at once, registers `EV_WRITE` (and the
     *          deadlines), or -- refused at once -- leaves its address for the next (see the class note).
     */
    void
    run() {
        QB_LOG_DEBUG("Started async connect to " << host_);
        // Apply the TLS verification policy before the (non-blocking) connect so
        // it is in effect when the handshake starts. No-op for plain sockets.
        if constexpr (requires { socket_.set_insecure(); }) {
            if (!verify_peer_)
                socket_.set_insecure();
        }
        own_fd_                 = !socket_.is_open();
        const bool by_endpoints = listed_ || ((endpoint_capable || Negotiator_::enabled) && remote_.af() != AF_UNIX);
        if (by_endpoints && !listed_)
            endpoints_ = qb::io::tcp::resolve_endpoints(remote_.af(), host_, remote_.u_port());
        if (by_endpoints && endpoints_.empty()) {
            QB_LOG_DEBUG("No address to connect to for " << host_);
            deliver_failure_deferred();
            return;
        }
        start_next();
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
        if (!(event._revents & (EV_READ | EV_WRITE)) || socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err))
            err = 1;

        if (!err || err == EISCONN) {
            ++attempt_; // the TCP connect is up: what follows is final, an attempt timer must not move on
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    listener::current.unregisterEvent(event._interface);
                    io_iface_ = nullptr;
                    if (!mark_completed_once())
                        return;
                    QB_LOG_DEBUG("Connected async to " << host_);
                    deliver(std::move(socket_));
                    return;
                case finalize_result::pending:
                    static_cast<event::io &>(const_cast<event::io &>(event)).set(EV_READ | EV_WRITE);
                    return;
                case finalize_result::failed:
                    break;
            }
            // Past the TCP connect (the TLS handshake): final -- the next address would present the same server.
            socket_.disconnect();
            listener::current.unregisterEvent(event._interface);
            io_iface_ = nullptr;
            if (!mark_completed_once())
                return;
            QB_LOG_DEBUG("Failed to finalize the connect to " << host_);
            deliver(Socket_{});
            return;
        }

        // The TCP connect failed: the next address, if there is one.
        listener::current.unregisterEvent(event._interface);
        io_iface_ = nullptr;
        QB_LOG_DEBUG("Async connect to " << host_ << " (address " << next_ << "/" << endpoints_.size() << ") failed err=" << err);
        start_next();
    }

    /**
     * @brief Deadline handler: unregister the write watcher if still registered,
     *        close the socket, and complete with failure if still the first completion.
     */
    void
    on_deadline() {
        ++attempt_; // an attempt timer still pending finds another generation
        if (io_iface_) {
            listener::current.unregisterEvent(io_iface_);
            io_iface_ = nullptr;
        }
        socket_.disconnect();

        if (!mark_completed_once())
            return;

        QB_LOG_DEBUG("Async connect deadline for " << host_);
        deliver(Socket_{});
    }

    /**
     * @brief One attempt's share of the deadline elapsed (`tcp::connect_attempt_budget`): its address is
     *        given up and the next is tried. Ignored when another attempt, or the end, came first.
     */
    void
    on_attempt_deadline(std::uint32_t const generation) {
        if (generation != attempt_ || completed_)
            return;
        if (io_iface_) {
            listener::current.unregisterEvent(io_iface_);
            io_iface_ = nullptr;
        }
        QB_LOG_DEBUG("Async connect to " << host_ << " (address " << next_ << "/" << endpoints_.size()
                                         << ") did not answer within its share of the deadline");
        start_next();
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

    /// SNI / verification hostname for the TLS upgrade (the remote URI host, or the endpoint list's name).
    std::string
    starttls_host() const {
        return host_;
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
    on_starttls(event::io const &event) {
        auto &mutable_event = const_cast<event::io &>(event);
        int   err           = 0;
        if (!(event._revents & (EV_READ | EV_WRITE)) || socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)
            || (err && err != EISCONN)) {
            if (sphase_ == sphase::connecting) { // the TCP connect itself failed: the next address, if any
                listener::current.unregisterEvent(event._interface);
                io_iface_ = nullptr;
                QB_LOG_DEBUG("STARTTLS connect to " << host_ << " failed err=" << err);
                start_next();
                return;
            }
            finish_starttls(event, false);
            return;
        }

        if (sphase_ == sphase::connecting)
            sphase_ = sphase::negotiating; // TCP connect just completed
        ++attempt_;                        // past the TCP connect: final, an attempt timer must not move on

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
 * @brief Initiates an asynchronous TCP connection to the first of `endpoints` that answers (since 3.3)
 *
 * The addresses are tried in order, exactly as the connector falls back through the ones a host name
 * resolves to (see `connector`): a refused, unrouted or silent address is closed and the next is tried,
 * each within its share of `timeout`; a TLS failure is final. The form for a caller that resolved the
 * name itself -- on the offload pool (`co_await offload(...)` around `qb::io::socket::resolve`), from a
 * cache, from service discovery.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type, invoked once with the connected socket or an empty one
 * @param endpoints Addresses to try, in order; an empty list completes with a failure
 * @param host Name for TLS -- SNI and certificate verification; ignored by a plain socket
 * @param func Callback function to call when connection completes
 * @param timeout The whole connect's deadline (`0` = none: each attempt waits as long as the system does)
 * @param verify_peer For secure transports, whether to verify the server certificate chain + `host`
 */
// Constrained like the uri overloads, for the same reason (see the note on the first one).
template <typename Socket_, typename Func_>
requires std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
connect(std::vector<qb::io::endpoint> endpoints, std::string host, Func_ &&func, qb::duration timeout = qb::duration::zero(),
        bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), std::move(endpoints), std::move(host),
                                                          qb::detail::to_ev_seconds(timeout), verify_peer);
    QB_LOG_DEBUG("Connector: Initializing over an endpoint list");
    op->run();
}

/**
 * @brief The endpoint-list connect with an existing socket (since 3.3)
 *
 * As `connect(endpoints, host, func, ...)`, with `existing_socket` moved into the connector first -- a
 * secure socket built from a `qb::io::ssl::Context` (custom trust, client certificate) keeps its policy.
 * A socket not open yet falls back through the list; one already open gets the first address only: its
 * descriptor may carry options a fresh one would not.
 */
template <typename Socket_, typename Func_>
requires std::invocable<std::remove_reference_t<Func_> &, Socket_ &&>
void
connect(Socket_ &&existing_socket, std::vector<qb::io::endpoint> endpoints, std::string host, Func_ &&func,
        qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), std::move(existing_socket), std::move(endpoints),
                                                          std::move(host), qb::detail::to_ev_seconds(timeout), verify_peer);
    QB_LOG_DEBUG("Connector: Initializing with existing socket over an endpoint list");
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

    uri                           _remote;
    std::vector<qb::io::endpoint> _endpoints; ///< the endpoint-list form (since 3.3): tried in order
    std::string                   _host;      ///< its TLS name
    bool                          _listed{false};
    qb::duration                  _timeout;
    bool                          _verify_peer{true};
    std::shared_ptr<state_t>      _state{std::make_shared<state_t>()};

public:
    explicit connect_awaiter(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true)
        : _remote(std::move(remote))
        , _timeout(timeout)
        , _verify_peer(verify_peer) {}

    /// The endpoint-list form (since 3.3): the first of `endpoints` that answers, `host` for TLS.
    connect_awaiter(std::vector<qb::io::endpoint> endpoints, std::string host, qb::duration timeout = qb::duration::zero(),
                    bool verify_peer = true)
        : _endpoints(std::move(endpoints))
        , _host(std::move(host))
        , _listed(true)
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

        auto state      = _state;
        auto on_connect = [state](Socket_ &&socket) {
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
            if (auto *target =
                    ::qb::io::async::CoroutineScheduler::current_ptr() ? ::qb::io::async::CoroutineScheduler::current_ptr() : state->scheduler;
                target && state->handle) {
                target->schedule_resume(state->handle);
            }
        };
        if (_listed)
            ::qb::io::async::tcp::connect<Socket_>(std::move(_endpoints), std::move(_host), std::move(on_connect), _timeout, _verify_peer);
        else
            ::qb::io::async::tcp::connect<Socket_>(_remote, std::move(on_connect), _timeout, _verify_peer);
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
 * @brief Factory for the endpoint-list connect awaiter (since 3.3): the first of `endpoints` that answers
 * @ingroup CoroutineTCP
 * @tparam Transport The transport type (default: transport::tcp)
 * @param endpoints Addresses to try, in order (see the callback form for the fallback rules)
 * @param host Name for TLS -- SNI and certificate verification; ignored by a plain socket
 * @param timeout The whole connect's deadline (default: none)
 * @code
 * auto eps  = co_await qb::io::async::offload([](std::string h) {   // resolve off the loop
 *     std::vector<qb::io::endpoint> out;
 *     qb::io::socket::resolve_v4(out, h.c_str(), 6379);
 *     return out;
 * }, std::string{"cache.internal"});
 * auto sock = co_await qb::io::async::tcp::connect(std::move(eps), "cache.internal", std::chrono::seconds{5});
 * @endcode
 */
template <typename Transport = qb::io::transport::tcp>
[[nodiscard]] auto
connect(std::vector<qb::io::endpoint> endpoints, std::string host, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    using socket_type = typename Transport::transport_io_type;
    return connect_awaiter<socket_type>{std::move(endpoints), std::move(host), timeout, verify_peer};
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
