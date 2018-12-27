//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "../actor.h"
#include "../../../modules/mqtt/session.h"

#include "events.h"

#ifndef CUBE_SESSION_MQTT_ACTOR_H
#define CUBE_SESSION_MQTT_ACTOR_H

namespace cube {
    namespace session {
        namespace mqtt {

            template <typename Derived>
            class Actor
                    : public cube::mqtt::Session<service::iopoll::Proxy, Derived>
                    , public cube::session::Actor<Derived> {
            public:
                constexpr static const service::iopoll::Type type = service::iopoll::READWRITE;
                constexpr static const bool has_keepalive = true;
                Actor() = default;
            };

        }
    }
}

#endif //CUBE_SESSION_MQTT_ACTOR_H
