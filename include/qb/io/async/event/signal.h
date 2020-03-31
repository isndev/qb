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

#ifndef QB_IO_ASYNC_EVENT_SIGNAL_H
#define QB_IO_ASYNC_EVENT_SIGNAL_H

#include "base.h"

namespace qb {
    namespace io {
        namespace async {
            namespace event {

                template<int _SIG = -1>
                struct signal : public base<ev::sig> {
                    using base_t = base<ev::sig>;

                    signal(ev::loop_ref loop) : base_t(loop) {
                        set(_SIG);
                    }
                };

                template<>
                struct signal<-1> : public base<ev::sig> {
                    using base_t = base<ev::sig>;

                    signal(ev::loop_ref loop) : base_t(loop) {}
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_EVENT_SIGNAL_H
