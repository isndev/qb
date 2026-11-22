/**
 * @file qb/io/async/io_handler.h
 * @brief Session management for the asynchronous IO framework
 *
 * This file defines the io_handler class which provides session management
 * functionality for asynchronous IO operations. It handles the registration,
 * tracking, and cleanup of IO sessions.
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

#ifndef QB_IO_ASYNC_IO_HANDLER_H
#define QB_IO_ASYNC_IO_HANDLER_H

#include <type_traits>
#include <vector>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/type_traits.h> // qb::has_on — the two dispatch if-constexpr below test it
#include <qb/uuid.h>
#include <qb/io/async/event/extracted.h>
#include <qb/io/async/teardown.h>
#include <qb/io/config.h>

namespace qb::io::async {

template <typename _Derived>
class with_timeout; // io.h: the inactivity timer a session may have (`use<T>::timeout`)

namespace inactivity_name_ {
/// Whether a session has a member NAMED `inactivity_timer_t`, whatever its access or kind (Huly QB-1248's sixteenth
/// independent review): lookup of the name in a class deriving from the session and from a fallback that declares it
/// is ambiguous exactly when the session has one -- lookup precedes access checking. A final session cannot be
/// derived from: the name is looked up in it directly, and only a public member type of that name is seen there --
/// refused unless it names a usable public base; any other member of that name is ignored (the seventeenth
/// and eighteenth). A session whose destructor is `final` is declared `final` itself (its destructor's `final` is then
/// redundant): the probe derives from it.
struct fallback {
    struct inactivity_timer_t {};
};
template <typename _Session>
struct probe
    : _Session
    , fallback {};
// A function template's deduction, not a requires-expression: MSVC 19.51 makes the ambiguity a hard error there (C2385).
template <typename _Probe>
std::false_type test(typename _Probe::inactivity_timer_t *); // found once: the fallback's alone
template <typename _Probe>
std::true_type test(...); // ambiguous: the session has one too
/// A final session's: the name looked up in it, a public member type alone seen.
template <typename _Session>
concept names_it_publicly = requires { typename _Session::inactivity_timer_t; };
template <typename _Session, bool = std::is_final_v<_Session>>
struct declared : decltype(test<probe<_Session>>(nullptr)) {};
template <typename _Session>
struct declared<_Session, true> : std::bool_constant<names_it_publicly<_Session>> {};
/// The `with_timeout` a named type is, or has as its one base, found by deduction: the timer the extraction stops,
/// through that base -- never a `setTimeout()` the named type declares of its own, which may reach another timer, or
/// none (the twenty-first independent review).
template <typename _Derived>
with_timeout<_Derived> &
as_timer(with_timeout<_Derived> &timer) noexcept {
    return timer;
}
/// The name a session declares is usable: it names a public base of the session -- an accessible, unambiguous one,
/// which a pointer to the session converts to (an alias of a reference to it too): never a type the session only
/// converts to, whose timer was stopped in place of the inactivity timer, nor the session itself (the twentieth
/// independent review) -- that is a `with_timeout`, or has exactly one as a base.
template <typename _Session>
concept usable =
    requires { typename _Session::inactivity_timer_t; }
    && (!std::is_same_v<std::remove_cvref_t<typename _Session::inactivity_timer_t>, _Session>)
    && std::is_convertible_v<_Session *, std::add_pointer_t<typename _Session::inactivity_timer_t>> && requires(_Session &timed) {
           inactivity_name_::as_timer(static_cast<typename _Session::inactivity_timer_t &>(timed)).setTimeout(qb::duration::zero());
       };
/// The concept read through a class template: every compiler checks the name's access from here, never from the
/// `io_handler` a session may befriend -- g++ and clang accepted a private name there, MSVC refused it (the twenty-first
/// independent review).
template <typename _Session>
struct usable_t : std::bool_constant<usable<_Session>> {};
} // namespace inactivity_name_

/**
 * @class io_handler
 * @ingroup Async
 * @brief Session manager for asynchronous IO
 *
 * This template class manages sessions for asynchronous IO operations.
 * It provides methods for registering, tracking, and unregistering sessions,
 * as well as utilities for broadcasting data to all or selected sessions.
 *
 * @note **Thread Safety:** This class is designed to be used within a single VirtualCore
 *       (single thread). The `_sessions` map is accessed only from the thread that owns
 *       the `io_handler` instance. When used with `qb-core`, each server actor should
 *       have its own `io_handler` instance on its assigned VirtualCore. If you need
 *       to use this class in a multi-threaded context, each thread must have its own
 *       instance and sessions must not be shared between threads.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Session The session class type
 */
template <typename _Derived, typename _Session>
class io_handler {
    /**
     * @brief Friendship declaration for the base_io_t of the session class
     * @details
     * This gives the base I/O class of the session access to private members
     * of the io_handler, particularly the disconnected() method. The base_io_t
     * is typically a specialization of the io<> template from one of:
     * - qb::io::async::io<_Derived>
     * - qb::io::async::file_watcher<_Derived>
     * - qb::io::async::directory_watcher<_Derived>
     * - qb::io::async::input<_Derived>
     * - qb::io::async::output<_Derived>
     * - qb::io::async::tcp::client<_Derived, _Transport, _Server>
     */
    friend typename _Session::base_io_t;

protected:
    /**
     * @brief Handle a disconnected session - removes it from the sessions map.
     *
     * Called internally by the session's `dispose()` when a session disconnects.
     * Derived classes that override `disconnected(uuid)` to add custom logic
     * **MUST** call this base implementation to ensure the session is removed
     * from `_sessions` and its shared_ptr is released.
     *
     * @code
     * void disconnected(qb::uuid id) {
     *     // custom logic...
     *     io_handler<MyServer, MySession>::disconnected(id); // required
     * }
     * @endcode
     *
     * @param ident The UUID of the disconnected session
     */
    void
    disconnected(uuid ident) {
        _sessions.erase(ident);
    }

public:
    /**
     * @brief Type alias for the map of sessions
     *
     * Maps session UUIDs to shared pointers of session objects.
     */
    using session_map_t = qb::unordered_map<uuid, std::shared_ptr<_Session>>;

private:
    session_map_t _sessions;                               /**< Map of active sessions */
    std::size_t   _max_sessions = QB_DEFAULT_MAX_SESSIONS; /**< Maximum number of sessions allowed */
    /**
     * @brief Reusable scratch buffer for `stream()` / `stream_if()` broadcast fan-outs.
     *
     * Broadcasting must snapshot active sessions (so that a handler that disconnects
     * *us* during iteration cannot invalidate the map). Keeping the snapshot vector as
     * a member lets us reuse its backing storage across broadcast calls instead of
     * reallocating on every fan-out.
     * @note Thread-affine: this handler is single-threaded by design (see class docs).
     */
    mutable std::vector<std::shared_ptr<_Session>> _broadcast_scratch;
    mutable bool                                   _broadcast_in_progress = false;

public:
    /**
     * @brief Type alias for the session class
     */
    using IOSession = _Session;

    /**
     * @brief Default constructor
     */
    io_handler() = default;

    /**
     * @brief Default destructor
     */
    ~io_handler() = default;

    /**
     * @brief Get the map of active sessions
     * @return Reference to the sessions map
     */
    [[nodiscard]] session_map_t &
    sessions() {
        return _sessions;
    }

    /**
     * @brief Get the current number of active sessions.
     * @return The number of sessions currently registered.
     */
    [[nodiscard]] std::size_t
    session_count() const noexcept {
        return _sessions.size();
    }

    /**
     * @brief Get the maximum number of sessions allowed.
     * @return The maximum number of sessions, or 0 if unlimited.
     */
    [[nodiscard]] std::size_t
    max_sessions() const noexcept {
        return _max_sessions;
    }

    /**
     * @brief Set the maximum number of sessions allowed.
     * @param max The maximum number of sessions. Set to 0 to disable the limit (not recommended for production).
     * @note This limit is checked when registering new sessions via `registerSession()`.
     *       If the limit is reached, `registerSession()` closes the incoming I/O and
     *       returns `nullptr` (it does not throw).
     */
    void
    set_max_sessions(std::size_t max) noexcept {
        _max_sessions = max;
    }

    /**
     * @brief Get a session by its UUID
     *
     * @param id The UUID of the session to retrieve
     * @return Shared pointer to the session, or nullptr if not found
     */
    [[nodiscard]] std::shared_ptr<_Session>
    session(uuid id) {
        auto it = _sessions.find(id);
        return (it != std::end(_sessions)) ? it->second : nullptr;
    }

    /**
     * @brief Register a new session.
     *
     * Creates and registers a new session backed by the given transport I/O object and
     * additional constructor arguments. The session is started immediately after
     * registration.
     *
     * @tparam Args Types of additional arguments forwarded to the session constructor.
     * @param new_io The transport I/O object (e.g. accepted socket) for the new session.
     * @param args  Additional arguments forwarded to the session constructor.
     * @return Pointer to the newly created session on success, or `nullptr` if the session
     *         limit has been reached (in which case `new_io` is closed and no session is
     *         allocated).
     *
     * @note **Session limit / DoS hardening.** If `_max_sessions > 0` and the registry is
     *       already full, this method closes `new_io` *before* any allocation takes place
     *       and returns `nullptr`. The previous implementation allocated and registered a
     *       `_Session` just to immediately `disconnect()` it, which amplified the cost of
     *       each rejected connection under a connection-flood scenario.
     *
     * @note **Usage.** The session limit can be configured via `set_max_sessions()` or by
     *       defining `QB_DEFAULT_MAX_SESSIONS` before including this header. The default is
     *       `0` (unlimited) for backward compatibility.
     *
     * @note **The session is constructed BEFORE it owns a socket.** The order below is
     *       fixed and is the only one possible — the session must exist before the accepted
     *       socket can be moved into it:
     *         1. `std::make_shared<_Session>(...)` — the session **constructor** runs here,
     *            against a default-constructed transport whose descriptor is `-1`;
     *         2. `session->transport() = std::move(new_io)` — the real socket arrives;
     *         3. `registered.start()` — the session is wired into the event loop;
     *         4. `_Derived::on(_Session&)` — called if the server defines it.
     *       So **anything that touches the descriptor must not go in the session's
     *       constructor**: `set_optval` (`TCP_NODELAY`, `SO_*`, keep-alive), `getsockname`,
     *       peer lookup and TLS handle access all silently address fd `-1` there — they
     *       fail, and nothing reports it. Do that work in the server's `on(_Session&)` hook
     *       (step 4), which exists for exactly this and runs with the socket installed.
     */
    template <typename... Args>
    _Session *
    registerSession(typename _Session::transport_io_type &&new_io, Args &&...args) {
        // DoS hardening: enforce the session cap *before* allocating the session.
        // `new_io` already owns an open FD (and possibly an `SSL*`), so closing it
        // here is O(1) and frees kernel resources immediately.
        if (_max_sessions > 0 && _sessions.size() >= _max_sessions) {
            new_io.close();
            return nullptr;
        }

        auto session        = std::make_shared<_Session>(static_cast<_Derived &>(*this), std::forward<Args>(args)...);
        auto [it, inserted] = _sessions.emplace(session->id(), session);

        if (!inserted) {
            new_io.close();
            return nullptr;
        }

        session->transport() = std::move(new_io);
        auto &registered     = *session;
        registered.start();
        if constexpr (qb::has_on<_Derived, _Session &>) {
            static_cast<_Derived &>(*this).on(registered);
            // The hook may have extracted or replaced the session: `session` alone keeps it alive
            // until this returns, so a pointer to it would dangle in the caller (Huly QB-306).
            const auto still = _sessions.find(registered.id());
            if (still == _sessions.cend() || still->second != session)
                return nullptr;
        }
        return &registered;
    }

    /**
     * @brief Unregister a session
     *
     * Disconnects and eventually removes the session with the given UUID.
     *
     * @param ident The UUID of the session to unregister
     */
    void
    unregisterSession(uuid const &ident) {
        auto it = _sessions.find(ident);
        if (it != _sessions.cend())
            it->second->disconnect();
    }

    /**
     * @brief Extract a session's IO object
     *
     * Removes the session with the given UUID and returns its IO object. The session stops watching the
     * descriptor before it gives it away, and its inactivity timer stops -- the one it names (a public type
     * `inactivity_timer_t` naming a public base that is a `with_timeout`, or has exactly one as a base, stopped through
     * that `with_timeout`: a base a pointer to the session converts to -- one naming the session itself, or a type it
     * only converts to, does not compile --; any other member of that name does not compile either, but in a final
     * session, where only a public member type of that name is seen; a session whose destructor is `final` is declared
     * `final`), else its public `use<T>::timeout` base, else a base template's; `getTimeout()` reads 0 after: one that
     * is still held somewhere is never dispatched for it again, nor timed out (its own `disconnect()` still ends it).
     * What it had read and not consumed stays in its buffer -- only the IO object leaves.
     *
     * @param ident The UUID of the session to extract
     * @return A pair containing the IO object and a boolean indicating success
     */
    [[nodiscard]] std::pair<typename _Session::transport_io_type, bool>
    extractSession(uuid const &ident) {
        const auto it = _sessions.find(ident);
        if (it == _sessions.cend())
            return {typename _Session::transport_io_type{}, false};
        auto keep_alive = it->second;
        // A session in a lingering close (io::close_after_deliver()) leaves it: its bound must not end the
        // session under its new owner (Huly QB-1089).
        if constexpr (requires { keep_alive->cancel_linger(); })
            keep_alive->cancel_linger();
        const bool queued = output_queued_(*keep_alive); // before a hook can empty the queue (Huly QB-1270)
        if constexpr (qb::has_on<_Session, qb::io::async::event::extracted>) {
            // The hook may register or extract sessions: no iterator survives it (a rehash frees the
            // bucket array, an erase frees the node -- Huly QB-306). Look the session up again.
            keep_alive->on(qb::io::async::event::extracted{});
            const auto still = _sessions.find(ident);
            if (still == _sessions.cend() || still->second != keep_alive)
                return {typename _Session::transport_io_type{}, false}; // extracted or replaced inside its hook
            auto t_io = take_transport_(*keep_alive, queued);
            _sessions.erase(still);
            return {std::move(t_io), true};
        } else {
            auto t_io = take_transport_(*keep_alive, queued);
            _sessions.erase(it);
            return {std::move(t_io), true};
        }
    }

private:
    /// Whether a session has output queued -- what it exposes of its queue: `pendingWrite()` is its base's.
    [[nodiscard]] static bool
    output_queued_([[maybe_unused]] _Session &session) noexcept {
        if constexpr (requires { session.out().size(); })
            return session.out().size() != 0;
        else
            return false;
    }

    /// The transport of a session that is being extracted. The session stops watching the descriptor first: it is its
    /// next owner's from here, and a session somebody still held -- a `shared_ptr` kept across a protocol upgrade --
    /// was dispatched for that owner's octets, failed the read of a transport it no longer had, and was told it had
    /// disconnected (Huly QB-1270). Output the session still has queued dies with it -- what was queued before its
    /// extracted hook ran (`queued`), or after: a hook that empties the queue drops that output all the same. A TLS
    /// stream is marked cut first, so that its next owner's close does not tell the peer it is whole (Huly QB-1270).
    [[nodiscard]] static typename _Session::transport_io_type
    take_transport_(_Session &session, bool queued) {
        // The base's own stop(), by name: a session may declare a `stop()` of its own that means something else.
        if constexpr (requires { static_cast<typename _Session::base_io_t &>(session).stop(); })
            static_cast<typename _Session::base_io_t &>(session).stop();
        // Its inactivity timer too: held past its extraction, its timeout ran and disconnected -- and told the server of
        // -- a session whose connection goes on under its next owner (Huly QB-1248's twelfth independent review). The
        // one the session names first (`inactivity_timer_t`: qbm-http's server sessions name their base template's, so
        // that a `use<S>::timeout` a session type keeps of its own is not taken for it -- the fourteenth), then the
        // `use<T>::timeout` base by its type, then the one a base template has, by deduction. Any other `with_timeout`
        // the session keeps, for something of its own, is left alone; a private one is out of reach, and stays armed.
        // Any other member of that name -- a private alias, a data member, a name inherited twice -- is a compile error:
        // ignored, it let another timer be stopped, or none (the fifteenth and sixteenth). So is a name for the session
        // itself, or for a type it only converts to: no base, a conversion reached another timer (the twentieth); and a
        // base that is no `with_timeout` and has none: its own `setTimeout()` reached another timer, or none (the
        // twenty-first) -- the named one is stopped through its `with_timeout`. A final session is out of the probe's
        // reach: there a public member type alone is seen (the seventeenth and eighteenth).
        if constexpr (inactivity_name_::declared<_Session>::value) {
            static_assert(inactivity_name_::usable_t<_Session>::value,
                          "inactivity_timer_t must name a public base of the session that is a with_timeout, or has exactly one: "
                          "the timer io_handler::extractSession() stops");
            if constexpr (inactivity_name_::usable_t<_Session>::value) // refused above: its message the only error
                inactivity_name_::as_timer(static_cast<typename _Session::inactivity_timer_t &>(session)).setTimeout(qb::duration::zero());
        } else if constexpr (std::is_base_of_v<with_timeout<_Session>, _Session>) {
            if constexpr (requires { static_cast<with_timeout<_Session> &>(session); })
                static_cast<with_timeout<_Session> &>(session).setTimeout(qb::duration::zero());
        } else if constexpr (requires { stop_timer_(session); }) {
            stop_timer_(session);
        }
        detail::mark_cut_stream(session.transport(), queued || output_queued_(session));
        return std::move(session.transport());
    }

    /// The inactivity timer a session has through a base template (a `with_timeout<B>` base for a B other than the
    /// session's own type), stopped through that base -- a session may declare a `setTimeout()` of its own that means
    /// something else. Two such bases make the deduction ambiguous: neither is stopped -- a session that keeps two
    /// names its inactivity timer (`inactivity_timer_t`).
    template <typename _Timed>
    static void
    stop_timer_(with_timeout<_Timed> &timer) noexcept {
        timer.setTimeout(qb::duration::zero());
    }

public:
    /**
     * @brief Broadcast data to all sessions
     *
     * Sends the provided data to all active sessions.
     *
     * @tparam _Args Types of data to send
     * @param args Data to send to all sessions
     * @return Reference to the derived object for method chaining
     */
    template <typename... _Args>
    _Derived &
    stream(_Args &&...args) {
        if (_broadcast_in_progress) {
            std::vector<std::shared_ptr<_Session>> local_snapshot;
            local_snapshot.reserve(_sessions.size());
            for (auto &[key, session] : _sessions)
                local_snapshot.push_back(session);
            for (auto &session : local_snapshot)
                (*session << ... << args);
            return static_cast<_Derived &>(*this);
        }

        _broadcast_in_progress = true;
        // Reuse the member scratch vector to amortise the allocation across broadcasts.
        // The snapshot is still mandatory to keep session shared-ptrs alive if a
        // handler disconnects the broadcaster's peers mid-iteration.
        try {
            _broadcast_scratch.clear();
            _broadcast_scratch.reserve(_sessions.size());
            for (auto &[key, session] : _sessions)
                _broadcast_scratch.push_back(session);
            for (auto &session : _broadcast_scratch)
                (*session << ... << args);
            _broadcast_scratch.clear();
            _broadcast_in_progress = false;
        } catch (...) {
            _broadcast_scratch.clear();
            _broadcast_in_progress = false;
            throw;
        }
        return static_cast<_Derived &>(*this);
    }

    /**
     * @brief Broadcast data to selected sessions
     *
     * Sends the provided data to sessions that match the given predicate.
     *
     * @tparam _Func Type of the selection predicate
     * @tparam _Args Types of data to send
     * @param func Predicate to select sessions to receive the data
     * @param args Data to send to selected sessions
     * @return Reference to the derived object for method chaining
     */
    template <typename _Func, typename... _Args>
    _Derived &
    stream_if(_Func const &func, _Args &&...args) {
        if (_broadcast_in_progress) {
            std::vector<std::shared_ptr<_Session>> local_snapshot;
            local_snapshot.reserve(_sessions.size());
            for (auto &[key, session] : _sessions)
                local_snapshot.push_back(session);
            for (auto &session : local_snapshot)
                if (func(*session))
                    (*session << ... << args);
            return static_cast<_Derived &>(*this);
        }

        _broadcast_in_progress = true;
        try {
            _broadcast_scratch.clear();
            _broadcast_scratch.reserve(_sessions.size());
            for (auto &[key, session] : _sessions)
                _broadcast_scratch.push_back(session);
            for (auto &session : _broadcast_scratch)
                if (func(*session))
                    (*session << ... << args);
            _broadcast_scratch.clear();
            _broadcast_in_progress = false;
        } catch (...) {
            _broadcast_scratch.clear();
            _broadcast_in_progress = false;
            throw;
        }
        return static_cast<_Derived &>(*this);
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_IO_HANDLER_H
