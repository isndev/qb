//
// Created by isndev on 12/5/18.
//

#include <cstdint>

#ifndef CUBE_SERVICE_MANAGER_TAGS_H
#define CUBE_SERVICE_MANAGER_TAGS_H

namespace cube {
    namespace service {
        namespace manager {
            struct Tag {
                static constexpr const uint16_t sid = 3;
            };

            struct AgentTag {
                static constexpr const uint16_t sid = 4;
            };
        }
    }
}

#endif //CUBE_SERVICE_MANAGER_TAGS_H
