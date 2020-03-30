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

#ifndef QB_IO_ASYNC_LISTENER_H_
#define QB_IO_ASYNC_LISTENER_H_

#include <vector>
#include <algorithm>
#include <thread>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include "event/base.h"

namespace qb {
    namespace io {
        namespace async {

            class listener {
            public:
                thread_local static listener current;
            private:

                template<typename _Event, typename _Actor>
                class RegisteredKernelEvent : public IRegisteredKernelEvent {
                    friend class listener;

                    _Actor &_actor;
                    _Event _event;

                    virtual ~RegisteredKernelEvent() = default;

                    explicit RegisteredKernelEvent(ev::loop_ref loop, _Actor &actor) noexcept
                            : _actor(actor), _event(loop) {}

                    virtual void invoke() override final {
                        if constexpr (has_member_func_is_alive<_Actor>::value) {
                            if (likely(_actor.is_alive()))
                                _actor.on(_event);
                        } else
                            _actor.on(_event);
                    }
                };

                ev::dynamic_loop _loop;
                std::vector<IRegisteredKernelEvent *> _registeredEvents;

            public:
                listener() : _loop(EVFLAG_AUTO) {}

                void clear() {
                    for (auto it : _registeredEvents)
                        delete it;
                    _registeredEvents.clear();
                }

                ~listener() {
                    clear();
                }


                template<typename EV_EVENT>
                void on(EV_EVENT &event, int revents) {
                    auto &w = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
                    w._revents = revents;
                    w._interface->invoke();
                }

                template<typename _Event, typename _Actor, typename ..._Args>
                _Event &registerEvent(_Actor &actor, _Args &&...args) {
                    auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
                    revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(this);
                    revent->_event._interface = revent;

                    if constexpr (sizeof...(_Args) > 0)
                        revent->_event.set(std::forward<_Args>(args)...);

                    _registeredEvents.push_back(revent);
                    return revent->_event;
                }

                void unregisterEvent(IRegisteredKernelEvent *kevent) {
                    _registeredEvents.erase(std::find(std::begin(_registeredEvents), std::end(_registeredEvents), kevent));
                    delete kevent;
                }

                inline ev::loop_ref loop() const {
                    return _loop;
                }

                inline void run(int flag = 0) {
                    _loop.run(flag);
                }

                inline std::size_t size() const {
                    return _registeredEvents.size();
                }
            };

            inline void init() {
                listener::current.clear();
            }

            inline void run(int flag = 0) {
                listener::current.run(flag);
            }

        }
    }
}

#endif //QB_IO_ASYNC_LISTENER_H_
