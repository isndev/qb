//
// Created by isndev on 12/5/18.
//

#include <cstdint>

#ifndef CUBE_SERVICE_SCHEDULER_TAGS_H
#define CUBE_SERVICE_SCHEDULER_TAGS_H

namespace cube {
    namespace service {
        namespace scheduler {
            struct TimerTag {
                static constexpr const uint16_t sid = 1;
            };

            struct TimeoutTag {
                static constexpr const uint16_t sid = 2;
            };
        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_TAGS_H
