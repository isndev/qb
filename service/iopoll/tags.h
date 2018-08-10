
#include "../../system/actor/ActorId.h"

#ifndef CUBE_SERVICE_IOPOLL_TAGS_H
#define CUBE_SERVICE_IOPOLL_TAGS_H

namespace cube {
    namespace service {
        namespace iopoll {
            template <std::size_t CoreIndex>
            struct Tags
            {
                constexpr static const uint8_t index = CoreIndex;
                constexpr static const uint16_t uid = 8;
                constexpr static ActorId id() { return ActorId(uid, CoreIndex); }
            };
        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_TAGS_H
