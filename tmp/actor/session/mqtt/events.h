//
// Created by isndev on 7/8/18.
//

#include "engine/actor/session/events.h"

#ifndef QB_SESSION_MQTT_EVENTS_H
#define QB_SESSION_MQTT_EVENTS_H

namespace qb {
	namespace session {
		namespace mqtt {
			namespace event {
				// input events
				using Ready = qb::service::iopoll::Proxy;
			}
		}
	}
}

#endif //QB_SESSION_MQTT_EVENTS_H
