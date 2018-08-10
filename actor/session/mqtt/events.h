//
// Created by isndev on 7/8/18.
//

#include "../events.h"

#ifndef CUBE_SESSION_MQTT_EVENTS_H
#define CUBE_SESSION_MQTT_EVENTS_H

namespace cube {
	namespace session {
		namespace mqtt {
			namespace event {
				// input events
				using Ready = cube::service::iopoll::event::Ready;
			}
		}
	}
}

#endif //CUBE_SESSION_MQTT_EVENTS_H
