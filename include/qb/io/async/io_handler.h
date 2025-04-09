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
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_IO_HANDLER_H
#define QB_IO_ASYNC_IO_HANDLER_H

#include <qb/system/container/unordered_map.h>
#include <qb/uuid.h>

namespace qb::io::async {

/**
 * @class io_handler
 * @brief Session manager for asynchronous IO
 *
 * This template class manages sessions for asynchronous IO operations.
 * It provides methods for registering, tracking, and unregistering sessions,
 * as well as utilities for broadcasting data to all or selected sessions.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Session The session class type
 */
template <typename _Derived, typename _Session>
class io_handler {
    friend typename _Session::base_io_t;

    /**
     * @brief Handle a disconnected session
     *
     * This method is called when a session is disconnected. It removes
     * the session from the sessions map.
     *
     * @param ident The UUID of the disconnected session
     */
    void
    disconnected(uuid ident) {
        const auto it = _sessions.find(ident);
        _sessions.erase(it);
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
    session_map_t &
    sessions() {
        return _sessions;
    }

    /**
     * @brief Get a session by its UUID
     *
     * @param id The UUID of the session to retrieve
     * @return Shared pointer to the session, or nullptr if not found
     */
    std::shared_ptr<_Session>
    session(uuid id) {
        auto it = _sessions.find(id);
        return (it != std::end(_sessions)) ? it->second : nullptr;
    }

    /**
     * @brief Register a new session
     *
     * Creates and registers a new session with the given IO object and
     * additional arguments. The session is started immediately after
     * registration.
     *
     * @tparam Args Types of additional arguments for session construction
     * @param new_io The IO object for the new session
     * @param args Additional arguments for session construction
     * @return Reference to the newly created session
     */
    template <typename... Args>
    _Session &
    registerSession(typename _Session::transport_io_type &&new_io, Args &&...args) {
        auto session = std::make_shared<_Session>(static_cast<_Derived &>(*this),
                                                  std::forward<Args>(args)...);
        sessions().emplace(session->id(), session);
        session->transport() = std::move(new_io);
        session->start();
        if constexpr (has_method_on<_Derived, void, _Session &>::value)
            static_cast<_Derived &>(*this).on(*session);
        return *session;
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
        for (auto &[key, session] : sessions())
            (*session << ... << std::forward<_Args>(args));
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
        for (auto &[key, session] : sessions())
            if (func(*session))
                (*session << ... << std::forward<_Args>(args));
        return static_cast<_Derived &>(*this);
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_IO_HANDLER_H
