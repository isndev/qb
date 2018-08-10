#include "../../system/actor/ActorId.h"

#ifndef CUBE_SERVICE_MANAGER_TAGS_H
#define CUBE_SERVICE_MANAGER_TAGS_H

namespace cube {
    namespace service {
        namespace manager {
            template <std::size_t CoreIndex>
            struct Tags
            {
                constexpr static const uint8_t index = CoreIndex;
                constexpr static const uint16_t uid = 3;
                constexpr static const uint16_t uid_agent = 4;

                constexpr static ActorId id() { return ActorId(uid, CoreIndex); }
                constexpr static ActorId id_agent() { return ActorId(uid_agent, CoreIndex); }
            };
        }
    }
}

#endif //CUBE_SERVICE_MANAGER_TAGS_H
