
#include "../../system/actor/Event.h"

#ifndef QB_SERVICE_MANAGER_EVENTS_H
#define QB_SERVICE_MANAGER_EVENTS_H

namespace qb {
    namespace service {
        namespace manager {
            namespace event {
                struct Base
                        : public ServiceEvent {
                };

                struct ToCore
                        : public Base {
                    uint16_t index = 0;
                    ToCore() {
                        service_event_id = type_id<ToCore>();
                    }
                };

                struct ToCoreRange
                        : public Base {
                    uint16_t begin = 0;
                    uint16_t end = 0;
                    ToCoreRange() {
                        service_event_id = type_id<ToCoreRange>();
                    }
                };
            }
        }
    }
}

#endif //QB_SERVICE_MANAGER_EVENTS_H
