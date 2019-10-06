//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_EVENT_TIMER_H
#define QB_IO_ASYNC_EVENT_TIMER_H

#include "base.h"

namespace qb {
    namespace io {
        namespace async {
            namespace event {

                struct timer : base<ev::timer> {
                    using base_t = base<ev::timer>;

                    timer(ev::loop_ref loop) : base_t(loop) {}
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_EVENT_TIMER_H
