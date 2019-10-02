//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_EVENT_FILE_H
#define QB_IO_ASYNC_EVENT_FILE_H

#include "base.h"

namespace qb {
    namespace io {
        namespace async {
            namespace event {

                struct file : base<ev::stat> {
                    using base_t = base<ev::stat>;

                    file(ev::loop_ref loop) : base_t(loop) {}
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_EVENT_FILE_H
