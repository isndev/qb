//
// Created by isndev on 7/8/18.
//

#include "engine/actor/session/events.h"

#ifndef CUBE_SESSION_LISTENER_EVENTS_H
#define CUBE_SESSION_LISTENER_EVENTS_H

namespace cube {
	namespace session {
		namespace listener {
			namespace event {
				// input events
				using Ready = cube::service::iopoll::Proxy;
				// output events
				using Subscribe = cube::service::iopoll::event::Subscribe;
			}
		}
	}
}

#endif //CUBE_SESSION_LISTENER_EVENTS_H
