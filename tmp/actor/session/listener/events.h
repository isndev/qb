//
// Created by isndev on 7/8/18.
//

#include "engine/actor/session/events.h"

#ifndef QB_SESSION_LISTENER_EVENTS_H
#define QB_SESSION_LISTENER_EVENTS_H

namespace qb {
	namespace session {
		namespace listener {
			namespace event {
				// input events
				using Ready = qb::service::iopoll::Proxy;
				// output events
				using Subscribe = qb::service::iopoll::event::Subscribe;
			}
		}
	}
}

#endif //QB_SESSION_LISTENER_EVENTS_H
