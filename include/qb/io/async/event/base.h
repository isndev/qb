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

#ifndef QB_IO_ASYNC_EVENT_BASE_H
#define QB_IO_ASYNC_EVENT_BASE_H

#include <ev/ev++.h>

namespace qb {
    namespace io {
        namespace async {

            class IRegisteredKernelEvent {
            public:
                virtual ~IRegisteredKernelEvent() {}

                virtual void invoke() = 0;
            };

            namespace event {

                template<typename _EV_EVENT>
                struct base : public _EV_EVENT {
                    using ev_t = _EV_EVENT;
                    IRegisteredKernelEvent *_interface;
                    int _revents;

                    base(ev::loop_ref loop) : _EV_EVENT(loop), _interface(nullptr), _revents(0) {}
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_EVENT_BASE_H
