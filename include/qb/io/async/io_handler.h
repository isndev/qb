/**
 * @file qb/io/async/io_handler.h
 * @brief Session management for the asynchronous IO framework
 *
 * This file defines the io_handler class which provides session management
 * functionality for asynchronous IO operations. It handles the registration,
 * tracking, and cleanup of IO sessions.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <vector>
#include <qb/system/container/unordered_map.h>
#include <qb/uuid.h>
#include <qb/io/async/event/extracted.h>
#include <qb/io/config.h>

namespace qb::io::async {

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
    session_map_t _sessions; /**< Map of active sessions */
    std::size_t _max_sessions = QB_DEFAULT_MAX_SESSIONS; /**< Maximum number of sessions allowed */
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
    mutable bool _broadcast_in_progress = false;

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
     *       If the limit is reached, `registerSession()` will throw a `std::runtime_error`.
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

        auto session = std::make_shared<_Session>(static_cast<_Derived &>(*this),
                                                  std::forward<Args>(args)...);
        session->transport() = std::move(new_io);
        auto [it, _] = _sessions.emplace(session->id(), std::move(session));

        auto &registered = *it->second;
        registered.start();
        if constexpr (qb::has_on<_Derived, _Session &>)
            static_cast<_Derived &>(*this).on(registered);
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
     * Removes the session with the given UUID and returns its IO object.
     *
     * @param ident The UUID of the session to extract
     * @return A pair containing the IO object and a boolean indicating success
     */
    [[nodiscard]] std::pair<typename _Session::transport_io_type, bool>
    extractSession(uuid const &ident) {
        auto it = _sessions.find(ident);
        if (it != _sessions.cend()) {
            if constexpr (qb::has_on<_Session, qb::io::async::event::extracted>)
                (*it->second).on(qb::io::async::event::extracted{});
            auto t_io = std::move(it->second->transport());
            _sessions.erase(it);
            return {std::move(t_io), true};
        }
        return {typename _Session::transport_io_type{}, false};
    }

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
