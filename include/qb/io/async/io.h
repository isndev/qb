//
// Created by isnDev on 9/17/2019.
//

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

                input() = delete;

                input(input const &) = delete;

                input(listener &listener)
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

                output() = delete;

                output(output const &) = delete;

                output(listener &listener)
                        : _listener(listener), _io_event(listener.registerEvent<event::io>(*this)) {}

                auto &out() {
                    return _prot.out();
                }

                void start() {
                    _prot.in().setBlocking(false);
                    _io_event.start(_prot.out().ident(), EV_WRITE);
                }

                inline char *push(char const *data, std::size_t size) {
                    if (!(_io_event.events & EV_WRITE))
                        _io_event.set(EV_WRITE);
                    return _prot.push(data, size);
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

                io() = delete;

                io(io const &) = delete;

                io(listener &listener)
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

                inline char *push(char const *data, std::size_t size) {
                    if (!(_io_event.events & EV_WRITE))
                        _io_event.set(EV_READ | EV_WRITE);
                    return _prot.push(data, size);
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

//                bool isAlive() const { return _io_event.is_active(); }
            };

        }
    }
}

#endif //QB_IO_ASYNC_IO_H
