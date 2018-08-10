
#include "../../system/actor/Event.h"

#ifndef CUBE_SERVICE_MANAGER_EVENTS_H
#define CUBE_SERVICE_MANAGER_EVENTS_H

namespace cube {
    namespace service {
        namespace manager {
            namespace event {
                struct Base
                        : public ServiceEvent {
                };

                // input/output events
                struct ToBestTimedCore
                        : public Base {
                    ToBestTimedCore() {
                        service_event_id = type_id<ToBestTimedCore>();
                    }
                };

                struct ToCore
                        : public Base {
                    uint16_t index = 0;
                    ToCore() {
                        service_event_id = type_id<ToCore>();
                    }
                };
            }
        }
    }
}

#endif //CUBE_SERVICE_MANAGER_EVENTS_H
