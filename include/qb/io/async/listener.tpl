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

#ifndef QB_IO_ASYNC_LISTENER_TPL
#define QB_IO_ASYNC_LISTENER_TPL

namespace qb {
    namespace io {
        namespace async {

//            thread_local struct ev::dynamic_loop Default::loop = { EVFLAG_AUTO };
//            thread_local listener listener::current;

//            thread_local listener listener::current;

//            template<typename EV_EVENT>
//            void listener::on(EV_EVENT &event, int revents) {
//                auto &w = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
//                w._revents = revents;
//                w._interface->invoke();
//            }
//
//            template<typename _Event, typename _Actor, typename ..._Args>
//            _Event &listener::registerEvent(_Actor &actor, _Args ...args) {
//                auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
//                revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(this);
//                revent->_event._interface = revent;
//
//                if constexpr (sizeof...(_Args) > 0)
//                    revent->_event.set(std::forward<_Args>(args)...);
//
//                _registeredEvents.push_back(revent);
//                std::cout << _registeredEvents.size() << " registered events" << std::endl;
//                return revent->_event;
//            }

        }
    }
}

#endif //QB_IO_ASYNC_LISTENER_TPL
