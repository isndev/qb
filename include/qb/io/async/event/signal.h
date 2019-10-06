//
// Created by isnDev on 9/17/2019.
//

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
