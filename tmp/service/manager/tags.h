//
// Created by isndev on 12/5/18.
//

#include <cstdint>

#ifndef QB_SERVICE_MANAGER_TAGS_H
#define QB_SERVICE_MANAGER_TAGS_H

namespace qb {
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

#endif //QB_SERVICE_MANAGER_TAGS_H
