
#include "system/actor/Event.h"

#ifndef CUBE_SERVICE_MANAGER_EVENTS_H
#define CUBE_SERVICE_MANAGER_EVENTS_H

namespace cube {
    namespace service {
        namespace manager {
            namespace event {
                struct Base
                        : public ServiceEvent {
                };

                // input events
                struct ToBestTimedCore
                        : public Base {
                    ToBestTimedCore() {
                        service_event_id = type_id<ToBestTimedCore>();
                    }
                };
            }
        }
    }
}

#endif //CUBE_SERVICE_MANAGER_EVENTS_H
