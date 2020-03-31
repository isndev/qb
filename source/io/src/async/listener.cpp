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

//#include <ev/ev.h>
//#include <ev/ev++.h>
#include            <qb/io/async/listener.h>

namespace qb {
    namespace io {
        namespace async {

//            thread_local struct ev::loop_ref Default::loop = ev_loop_new(EVFLAG_AUTO);
            thread_local listener listener::current = {};

//            thread_local listener &listener::current = *new listener();
//
//            listener::listener() : _loop(EVFLAG_AUTO) {}
//
//            listener::~listener() {
//                for (auto it : _registeredEvents)
//                    delete it;
//            }
//
//            void listener::unregisterEvent(IRegisteredKernelEvent *kevent) {
//                _registeredEvents.erase(std::find(std::begin(_registeredEvents), std::end(_registeredEvents), kevent));
//                delete kevent;
//            }

        } // namespace async
    } // namespace io
} // namespace qb
