/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_IO_ASYNC_IO_HANDLER_H
#define QB_IO_ASYNC_IO_HANDLER_H

#include <qb/system/container/unordered_map.h>
#include <qb/uuid.h>

namespace qb::io::async {

template <typename _Derived, typename _Session>
class io_handler {
    friend typename _Session::base_io_t;

    void
    disconnected(uuid ident) {
        const auto it = _sessions.find(ident);
        delete &it->second;
        _sessions.erase(it);
    }

public:
    using session_map_t = qb::unordered_map<uuid, _Session &>;

private:
    session_map_t _sessions;

public:
    using IOSession = _Session;

    io_handler() = default;
    ~io_handler() {
        for (auto &[key, session] : _sessions)
            delete &session;
    }

    session_map_t &
    sessions() {
        return _sessions;
    }

    template <typename... Args>
    _Session &
    registerSession(typename _Session::transport_io_type &&new_io, Args &&...args) {
        auto &session =
            *new _Session{static_cast<_Derived &>(*this), std::forward<Args>(args)...};
        const auto &it = sessions().emplace(session.id(), std::ref(session));
        it.first->second.transport() = std::move(new_io);
        it.first->second.start();
        if constexpr (has_method_on<_Derived, void, _Session &>::value)
            static_cast<_Derived &>(*this).on(it.first->second);
        return it.first->second;
    }

    _Session &
    registerSession(typename _Session::transport_io_type &&new_io) {
        auto &session = *new _Session{static_cast<_Derived &>(*this)};
        const auto &it = sessions().emplace(session.id(), std::ref(session));
        it.first->second.transport() = std::move(new_io);
        it.first->second.start();
        if constexpr (has_method_on<_Derived, void, _Session &>::value)
            static_cast<_Derived &>(*this).on(it.first->second);
        return it.first->second;
    }

    void
    unregisterSession(uuid const &ident) {
        auto it = _sessions.find(ident);
        if (it != _sessions.cend())
            it->second.disconnect();
    }

    [[nodiscard]] std::pair<typename _Session::transport_io_type, bool>
    extractSession(uuid const &ident) {
        auto it = _sessions.find(ident);
        if (it != _sessions.cend()) {
            auto t_io = std::move(it->second.transport());
            delete &it->second;
            _sessions.erase(it);
            return {std::move(t_io), true};
        }
        return {typename _Session::transport_io_type{}, false};
    }

    template <typename... _Args>
    _Derived &
    stream(_Args &&...args) {
        for (auto &session : sessions())
            (session.second << ... << std::forward<_Args>(args));
        return static_cast<_Derived &>(*this);
    }

    template <typename _Func, typename... _Args>
    _Derived &
    stream_if(_Func const &func, _Args &&...args) {
        for (auto &session : sessions())
            if (func(session.second))
                (session.second << ... << std::forward<_Args>(args));
        return static_cast<_Derived &>(*this);
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_IO_HANDLER_H
