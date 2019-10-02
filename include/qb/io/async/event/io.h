//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_EVENT_IO_H
#define QB_IO_ASYNC_EVENT_IO_H

#include "base.h"

namespace qb {
    namespace io {
        namespace async {
            namespace event {

                struct io : base<ev::io> {
                    using base_t = base<ev::io>;

                    io(ev::loop_ref loop) : base_t(loop) {}
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_EVENT_IO_H
