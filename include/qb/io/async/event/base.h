//
// Created by isnDev on 9/17/2019.
//

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
