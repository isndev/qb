/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#include "event/all.h"
#include "listener.h"
#include "protocol.h"
#include <qb/utility/type_traits.h>

CREATE_MEMBER_CHECK(Protocol);
GENERATE_HAS_METHOD(flush)

namespace qb::io::async {

template <typename _Derived, typename _EV_EVENT>
class base {
protected:
    _EV_EVENT &_async_event;
    base()
        : _async_event(listener::current.registerEvent<_EV_EVENT>(
              static_cast<_Derived &>(*this))) {}
    ~base() {
        listener::current.unregisterEvent(_async_event._interface);
    }
};

template <typename _Derived>
class with_timeout : public base<with_timeout<_Derived>, event::timer> {
    double _timeout;
    double _last_activity;

public:
    explicit with_timeout(double timeout = 3)
        : _timeout(timeout)
        , _last_activity(0.) {
        if (timeout > 0.)
            this->_async_event.start(_timeout);
    }

    void
    updateTimeout() noexcept {
        _last_activity = this->_async_event.loop.now();
    }

    void
    setTimeout(double timeout) noexcept {
        _timeout = timeout;
        if (_timeout) {
            _last_activity = this->_async_event.loop.now();
            this->_async_event.set(_timeout);
            this->_async_event.start();
        } else
            this->_async_event.stop();
    }

private:
    friend class listener::RegisteredKernelEvent<event::timer, with_timeout>;

    void
    on(event::timer &event) noexcept {
        const ev_tstamp after = _last_activity - event.loop.now() + _timeout;

        if (after < 0.)
            static_cast<_Derived &>(*this).on(event);
        else {
            this->_async_event.set(after);
            this->_async_event.start();
        }
    }
};

#define Derived static_cast<_Derived &>(*this)

template <typename _Derived>
class input_file : public base<input_file<_Derived>, event::file> {
    using base_t = base<input_file<_Derived>, event::file>;
    AProtocol<_Derived> *_protocol = nullptr;
    std::vector<AProtocol<_Derived> *> _protocol_list;

public:
    using base_io_t = input_file<_Derived>;

    input_file() = default;
    input_file(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {}
    input_file(input_file const &) = delete;
    ~input_file() noexcept {
        for (auto protocol : _protocol_list)
            delete protocol;
    }

    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&... args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
        }
        return nullptr;
    }

    void
    start(std::string const &fpath, ev_tstamp ts = 0.1) noexcept {
        this->_async_event.start(fpath.c_str(), ts);
    }

    void
    disconnect() noexcept {
        this->_async_event.stop();
    }

private:
    friend class listener::RegisteredKernelEvent<event::file, input_file>;

    void
    on(event::file const &event) {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;

        if (!_protocol->ok() || !event.attr.st_nlink)
            goto error;
        if (event.prev.st_size != event.attr.st_size) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                // has a new message to read
                this->_protocol->onMessage(ret);
                Derived.flush(ret);
            }
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
                    }
                }
            }
        }
        return;
    error:
        this->_async_event.stop();
        Derived.close();
    }
};

template <typename _Derived>
class input : public base<input<_Derived>, event::io> {
    using base_t = base<input<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr;
    std::vector<AProtocol<_Derived> *> _protocol_list;
    bool _disconnected_by_user = false;

public:
    using base_io_t = input<_Derived>;
    constexpr static const bool has_server = false;

    input() = default;
    input(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {}
    input(input const &) = delete;
    ~input() noexcept {
        for (auto protocol : _protocol_list)
            delete protocol;
    }

    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&... args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
        }
        return nullptr;
    }

    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().setBlocking(false);
        this->_async_event.start(Derived.transport().fd(), EV_READ);
    }

    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ))
            this->_async_event.start(Derived.transport().fd(), EV_READ);
    }

    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            event::disconnected e;
            e.reason = reason;
            Derived.on(e);
        }
        _disconnected_by_user = true;
        listener::current.loop().feed_fd_event(Derived.transport().fd(), EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, input>;

    void
    on(event::io const &event) {
        constexpr const auto invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;

        if (_disconnected_by_user || !_protocol->ok())
            goto error;

        if (likely(event._revents & EV_READ)) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                // has a new message to read
                this->_protocol->onMessage(ret);
                Derived.flush(ret);
            }
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
                    }
                }
            }
        }

        if (!(event._revents & EV_ERROR))
            return;
    error:
        if (!_disconnected_by_user)
            disconnect();
        this->_async_event.stop();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(event.fd);
        }
    }
};

template <typename _Derived>
class output : public base<output<_Derived>, event::io> {
    using base_t = base<output<_Derived>, event::io>;
    bool _disconnected_by_user = false;

public:
    using base_io_t = output<_Derived>;
    constexpr static const bool has_server = false;

    output() = default;
    output(output const &) = delete;
    ~output() = default;

    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().setBlocking(false);
        this->_async_event.start(Derived.transport().fd(), EV_WRITE);
    }

    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.set(EV_WRITE);
    }

    template <typename... _Args>
    inline auto &
    publish(_Args &&... args) noexcept {
        ready_to_write();
        if constexpr (sizeof...(_Args))
            (Derived.out() << ... << std::forward<_Args>(args));
        return Derived.out();
    }

    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            event::disconnected e;
            e.reason = reason;
            Derived.on(e);
        }
        _disconnected_by_user = true;
        listener::current.loop().feed_fd_event(Derived.transport().fd(), EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, output>;

    void
    on(event::io const &event) {
        auto ret = 0;

        if (_disconnected_by_user)
            goto error;

        if (likely(event._revents & EV_WRITE)) {
            ret = Derived.write();
            if (unlikely(ret < 0))
                goto error;
            if (!Derived.pendingWrite()) {
                this->_async_event.stop(EV_NONE);
                if constexpr (has_method_on<_Derived, void, event::eos>::value) {
                    Derived.on(event::eos{});
                }
            } else if constexpr (has_method_on<_Derived, void,
                                               event::pending_write>::value) {
                Derived.on(event::pending_write{Derived.pendingWrite()});
            }
        }

        if (!(event._revents & EV_ERROR))
            return;
    error:
        if (!_disconnected_by_user)
            disconnect();
        this->_async_event.stop();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(event.fd);
        }
    }
};

template <typename _Derived>
class io : public base<io<_Derived>, event::io> {
    using base_t = base<io<_Derived>, event::io>;
    AProtocol<_Derived> *_protocol = nullptr;
    std::vector<AProtocol<_Derived> *> _protocol_list;
    bool _disconnected_by_user = false;

public:
    using base_io_t = io<_Derived>;
    constexpr static const bool has_server = false;

    io() = default;
    io(AProtocol<_Derived> *protocol) noexcept
        : _protocol(protocol) {}
    io(io const &) = delete;
    ~io() noexcept {
        for (auto protocol : _protocol_list)
            delete protocol;
    }

    template <typename _Protocol, typename... _Args>
    _Protocol *
    switch_protocol(_Args &&... args) {
        auto new_protocol = new _Protocol(std::forward<_Args>(args)...);
        if (new_protocol->ok()) {
            _protocol = new_protocol;
            _protocol_list.push_back(new_protocol);
            return new_protocol;
        } else
            delete new_protocol;
        return nullptr;
    }

    void
    start() noexcept {
        _disconnected_by_user = false;
        Derived.transport().setBlocking(false);
        this->_async_event.start(Derived.transport().fd(), EV_READ);
    }

    void
    ready_to_read() noexcept {
        if (!(this->_async_event.events & EV_READ))
            this->_async_event.set(this->_async_event.events | EV_READ);
    }

    void
    ready_to_write() noexcept {
        if (!(this->_async_event.events & EV_WRITE))
            this->_async_event.set(this->_async_event.events | EV_WRITE);
    }

    void
    close_after_deliver() const noexcept {
        _protocol->not_ok();
    }

    template <typename... _Args>
    inline auto &
    publish(_Args &&... args) noexcept {
        ready_to_write();
        if constexpr (sizeof...(_Args))
            (Derived.out() << ... << std::forward<_Args>(args));
        return Derived.out();
    }

    template <typename T>
    auto &
    operator<<(T &&data) {
        return publish(std::forward<T>(data));
    }

    void
    disconnect(int reason = 0) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value) {
            event::disconnected e;
            e.reason = reason;
            Derived.on(e);
        }
        _disconnected_by_user = true;
        listener::current.loop().feed_fd_event(Derived.transport().fd(), EV_UNDEF);
    }

private:
    friend class listener::RegisteredKernelEvent<event::io, io>;

    void
    on(event::io const &event) {
        constexpr const std::size_t invalid_ret = static_cast<std::size_t>(-1);
        std::size_t ret = 0u;

        if (_disconnected_by_user)
            goto error;

        if (event._revents & EV_READ && _protocol->ok()) {
            ret = static_cast<std::size_t>(Derived.read());
            if (unlikely(ret == invalid_ret))
                goto error;
            while ((ret = this->_protocol->getMessageSize()) > 0) {
                this->_protocol->onMessage(ret);
                Derived.flush(ret);
            }
            Derived.eof();
            if constexpr (has_method_on<_Derived, void, event::pending_read>::value ||
                          has_method_on<_Derived, void, event::eof>::value) {
                const auto pendingRead = Derived.pendingRead();
                if (pendingRead) {
                    if constexpr (has_method_on<_Derived, void,
                                                event::pending_read>::value) {
                        Derived.on(event::pending_read{pendingRead});
                    }
                } else {
                    if constexpr (has_method_on<_Derived, void, event::eof>::value) {
                        Derived.on(event::eof{});
                    }
                }
            }
        }
        if (event._revents & EV_WRITE) {
            ret = static_cast<std::size_t>(Derived.write());
            if (unlikely(ret == invalid_ret))
                goto error;
            if (!Derived.pendingWrite()) {
                if (!_protocol->ok())
                    goto error;
                this->_async_event.set(EV_READ);
                if constexpr (has_method_on<_Derived, void, event::eos>::value) {
                    Derived.on(event::eos{});
                }
            } else if constexpr (has_method_on<_Derived, void,
                                               event::pending_write>::value) {
                Derived.on(event::pending_write{Derived.pendingWrite()});
            }
        }
        if (!(event._revents & EV_ERROR))
            return;
    error:
        if (!_disconnected_by_user)
            disconnect();
        this->_async_event.stop();
        Derived.close();
        if constexpr (_Derived::has_server) {
            Derived.server().disconnected(event.fd);
        }
    }
};

} // namespace qb::io::async

#undef Derived
#endif // QB_IO_ASYNC_IO_H
