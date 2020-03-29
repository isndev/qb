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

#include <qb/utility/type_traits.h>
#include "listener.h"
#include "event/io.h"
#include "event/eos.h"
#include "event/disconnected.h"
#include "event/timer.h"

GENERATE_HAS_METHOD(on)

namespace qb {
    namespace io {
        namespace async {

            template <typename _Derived, typename _EV_EVENT>
            class base {
            protected:
                _EV_EVENT &_async_event;
                base() : _async_event(listener::current.registerEvent<_EV_EVENT>(static_cast<_Derived &>(*this))) {}
                ~base() {
                    listener::current.unregisterEvent(_async_event._interface);
                }
            };

            template <typename _Derived>
            class with_timeout : public base<with_timeout<_Derived>, event::timer> {
                double _timeout;
                double _last_activity;
            public:
                with_timeout(double timeout = 3)
                    : _timeout(timeout)
                    , _last_activity(0.) {
                    if (timeout > 0.)
                        this->_async_event.start(_timeout);
                }

                void updateTimeout() {
                    _last_activity = this->_async_event.loop.now() + _timeout;
                }

                void setTimeout(double timeout) {
                    _timeout = timeout;
                    if (_timeout) {
                        _last_activity = this->_async_event.loop.now();
                        this->_async_event.set(_timeout);
                        this->_async_event.start();
                    } else
                        this->_async_event.stop();
                }

                void on(event::timer &event) {
                    const ev_tstamp after = _last_activity - event.loop.now() + _timeout;

                    if (after < 0.)
                        static_cast<_Derived &>(*this).on(event);
                    else {
                        this->_async_event.set(after);
                        this->_async_event.start();
                    }
                }
            };

            template<typename _Derived, typename _Prot>
            class input : public base<input<_Derived, _Prot>, event::io> {
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;
                using IOMessage = typename _Prot::message_type;

                input() = default;
                input(input const &) = delete;

                auto &in() {
                    return _prot.in();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    this->_async_event.start(_prot.in().fd(), EV_READ);
                }

                void disconnect(int reason = 0) {
                    this->_async_event.stop();

                    if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
                        event::disconnected e;
                        e.reason = reason;
                        static_cast<_Derived &>(*this).on(e);
                    }
                    const auto ident = _prot.in().ident();
                    _prot.close();
                    if constexpr (_Derived::has_server) {
                        static_cast<_Derived &>(*this).server().disconnected(ident);
                    }
                }

                void on(event::io const &event) {
                    auto ret = 0;
                    if (likely(event._revents & EV_READ)) {
                        ret = _prot.read();
                        if (unlikely(ret < 0))
                            goto error;
                        while ((ret = _prot.getMessageSize()) > 0) {
                            static_cast<_Derived &>(*this).on(_prot.getMessage(ret), ret);
                            _prot.flush(ret);
                        }
                    }
                    return;
                    error:
                    disconnect();
                }

            };

            template<typename _Derived, typename _Prot>
            class output : public base<output<_Derived, _Prot>, event::io> {
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;

                output() = default;
                output(output const &) = delete;

                auto &out() {
                    return _prot.out();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    this->_async_event.start(_prot.out().fd(), EV_WRITE);
                }

                template <typename ..._Args>
                inline auto publish(_Args &&...args) {
                    if (!(this->_async_event.events & EV_WRITE))
                        this->_async_event.set(EV_READ | EV_WRITE);
                    return _prot.publish(std::forward<_Args>(args)...);
                }

                void disconnect(int reason = 0) {
                    this->_async_event.stop();
                    if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
                        event::disconnected e;
                        e.reason = reason;
                        static_cast<_Derived &>(*this).on(e);
                    }
                    const auto ident = _prot.in().ident();
                    _prot.close();
                    if constexpr (_Derived::has_server) {
                        static_cast<_Derived &>(*this).server().disconnected(ident);
                    }
                }

                void on(event::io &event) {
                    auto ret = 0;
                    if (likely(event._revents & EV_WRITE)) {
                        ret = _prot.write();
                        if (unlikely(ret < 0))
                            goto error;
                        if (!_prot.pendingWrite()) {
                            event.stop();
                            if constexpr (has_method_on<_Derived, void, event::eos>::value) {
                                static_cast<_Derived &>(*this).on(event::eos{});
                            }
                        }
                    }
                    return;
                    error:
                    disconnect();
                }

            };

            template<typename _Derived, typename _Prot>
            class io : public base<io<_Derived, _Prot>, event::io> {
                _Prot _prot;
            public:
                constexpr static const bool has_server = false;
                using IOMessage = typename _Prot::message_type;

                io() = default;
                io(io const &) = delete;

                auto &in() {
                    return _prot.in();
                }

                auto &out() {
                    return _prot.out();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    _prot.out() = _prot.in();
                    this->_async_event.start(_prot.in().fd(), EV_READ);
                }

                template <typename ..._Args>
                inline auto publish(_Args &&...args) {
                    if (!(this->_async_event.events & EV_WRITE))
                        this->_async_event.set(EV_READ | EV_WRITE);
                    return _prot.publish(std::forward<_Args>(args)...);
                }

                void disconnect(int reason = 0) {
                    this->_async_event.stop();
                    if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
                        event::disconnected e;
                        e.reason = reason;
                        static_cast<_Derived &>(*this).on(e);
                    }
                    const auto ident = _prot.in().ident();
                    _prot.close();
                    if constexpr (_Derived::has_server) {
                        static_cast<_Derived &>(*this).server().disconnected(ident);
                    }
                }

                void on(event::io &event) {
                    auto ret = 0;
                    if (event._revents & EV_READ) {
                        ret = _prot.read();
                        if (unlikely(ret < 0))
                            goto error;
                        while ((ret = _prot.getMessageSize()) > 0) {
                            static_cast<_Derived &>(*this).on(_prot.getMessage(ret), ret);
                            _prot.flush(ret);
                        }
                    }
                    if (event._revents & EV_WRITE) {
                        ret = _prot.write();
                        if (unlikely(ret < 0))
                            goto error;
                        if (!_prot.pendingWrite()) {
                            event.set(EV_READ);
                            if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
                                static_cast<_Derived &>(*this).on(event::eos{});
                            }
                        }
                    }
                    return;
                    error:
                    disconnect();
                }
            };

        }
    }
}

#endif //QB_IO_ASYNC_IO_H
