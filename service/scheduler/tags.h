#include "../../system/actor/ActorId.h"

#ifndef CUBE_SERVICE_SCHEDULER_TAGS_H
#define CUBE_SERVICE_SCHEDULER_TAGS_H

namespace cube {
    namespace service {
        namespace scheduler {
            template <std::size_t CoreIndex>
            struct Tags
            {
                constexpr static const uint8_t index = CoreIndex;
                constexpr static const uint16_t uid_timer = 1;
                constexpr static const uint16_t uid_timeout = 2;

                constexpr static ActorId id_timer() { return ActorId(uid_timer, CoreIndex); }
                constexpr static ActorId id_timeout() { return ActorId(uid_timeout, CoreIndex); }
            };
        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_TAGS_H
