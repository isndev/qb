//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "engine/actor/session/actor.h"
#include "modules/mqtt/session.h"

#include "events.h"

#ifndef QB_SESSION_MQTT_ACTOR_H
#define QB_SESSION_MQTT_ACTOR_H

namespace qb {
    namespace session {
        namespace mqtt {

            template <typename Derived>
            class Actor
                    : public qb::mqtt::Session<service::iopoll::Proxy, Derived>
                    , public qb::session::Actor<Derived> {
            public:
                constexpr static const service::iopoll::Type type = service::iopoll::READWRITE;
                constexpr static const bool has_keepalive = true;
                Actor() = default;
            };

        }
    }
}

#endif //QB_SESSION_MQTT_ACTOR_H
