/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QB_IO_ASYNC_IO_H
#define QB_IO_ASYNC_IO_H

#include <type_traits>
#include "listener.h"
#include "event/io.h"

namespace qb {
    namespace io {
        namespace async {

            template<typename _Derived, typename _Prot>
            class input {
            protected:
                listener &_listener;
            private:
                event::io &_io_event;
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;

                input(input const &) = delete;

                input(listener &listener = listener::current)
                        : _listener(listener), _io_event(listener.registerEvent<event::io>(*this)) {}

                auto &in() {
                    return _prot.in();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    _io_event.start(_prot.in().ident(), EV_READ);
                }

                void on(event::io &event) {
                    auto ret = 0;
                    if (likely(event._revents & EV_READ)) {
                        ret = _prot.read();
                        if (unlikely(ret <= 0))
                            goto error;
                        while ((ret = _prot.getMessageSize()) > 0) {
                            static_cast<_Derived &>(*this).on(_prot.getMessage(), ret);
                            _prot.flush(ret);
                        }
                    }
                    return;
                    error:
                    event.stop();
                    if (static_cast<_Derived &>(*this).disconnected()) {
                        const auto ident = _prot.in().ident();
                        _listener.unregisterEvent(event._interface);
                        _prot.close();
                        if constexpr (_Derived::has_server) {
                            static_cast<_Derived &>(*this).server().disconnected(ident);
                        }
                    }
                }

                bool disconnected() const { return true; }

//                bool isAlive() const { return _io_event.is_active(); }
            };

            template<typename _Derived, typename _Prot>
            class output {
            protected:
                listener &_listener;
            private:
                event::io &_io_event;
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;

                output(output const &) = delete;

                output(listener &listener = listener::current)
                        : _listener(listener), _io_event(listener.registerEvent<event::io>(*this)) {}

                auto &out() {
                    return _prot.out();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    _io_event.start(_prot.out().ident(), EV_WRITE);
                }

                template <typename ..._Args>
                inline auto publish(_Args ...args) {
                    if (!(_io_event.events & EV_WRITE))
                        _io_event.set(EV_READ | EV_WRITE);
                    return _prot.publish(std::forward<_Args>(args)...);
                }

                void on(event::io &event) {
                    auto ret = 0;
                    if (likely(event._revents & EV_WRITE)) {
                        ret = _prot.write();
                        if (unlikely(ret <= 0))
                            goto error;
                        if (!_prot.pendingWrite())
                            _io_event.stop();
                    }
                    return;
                    error:
                    event.stop();
                    if (static_cast<_Derived &>(*this).disconnected()) {
                        const auto ident = _prot.out().ident();
                        _listener.unregisterEvent(event._interface);
                        _prot.close();
                        if constexpr (_Derived::has_server) {
                            static_cast<_Derived &>(*this).server().disconnected(ident);
                        }
                    }
                }

                bool disconnected() const { return true; }

//                bool isAlive() const { return _io_event.is_active(); }
            };

            template<typename _Derived, typename _Prot>
            class io {
            protected:
                listener &_listener;
            private:
                event::io &_io_event;
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;

                io(io const &) = delete;

                io(listener &listener = listener::current)
                        : _listener(listener), _io_event(listener.registerEvent<event::io>(*this)) {}

                auto &in() {
                    return _prot.in();
                }

                auto &out() {
                    return _prot.out();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    _prot.out() = _prot.in();
                    _io_event.start(_prot.in().ident(), EV_READ);
                }

                template <typename ..._Args>
                inline auto publish(_Args ...args) {
                    if (!(_io_event.events & EV_WRITE))
                        _io_event.set(EV_READ | EV_WRITE);
                    return _prot.publish(std::forward<_Args>(args)...);
                }

                void on(event::io &event) {
                    auto ret = 0;
                    if (event._revents & EV_READ) {
                        ret = _prot.read();
                        if (unlikely(ret <= 0))
                            goto error;
                        while ((ret = _prot.getMessageSize()) > 0) {
                            static_cast<_Derived &>(*this).on(_prot.getMessage(), ret);
                            _prot.flush(ret);
                        }
                    }
                    if (event._revents & EV_WRITE) {
                        ret = _prot.write();
                        if (unlikely(ret <= 0))
                            goto error;
                        if (!_prot.pendingWrite())
                            _io_event.set(EV_READ);
                    }
                    return;
                    error:
                    event.stop();
                    if (static_cast<_Derived &>(*this).disconnected()) {
                        const auto ident = _prot.in().ident();
                        _listener.unregisterEvent(event._interface);
                        _prot.close();
                        if constexpr (_Derived::has_server) {
                            static_cast<_Derived &>(*this).server().disconnected(ident);
                        }
                    }
                }

                bool disconnected() const { return true; }

//                bool isAlive() const { return _io_event.is_active(); }
            };

        }
    }
}

#endif //QB_IO_ASYNC_IO_H
