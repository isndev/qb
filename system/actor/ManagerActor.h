//
// Created by moham on 6/18/2018.
//

#ifndef CUBE_MANAGERACTOR_H
#define CUBE_MANAGERACTOR_H

#include <thread>
#include <algorithm>
#include "Actor.h"
#include "Event.h"
#include "system/Types.h"

namespace cube {

    struct ManagerBaseEvent
            : public ServiceEvent
    {
    };

    struct BestCoreEvent : public ManagerBaseEvent {
        BestCoreEvent() {
            service_event_id = type_id<BestCoreEvent>();
        }
    };


    namespace service {
        template<typename _CoreHandler>
        class ManagerAgentActor
                : public ServiceActor<_CoreHandler, 4> {
        };

        template<typename _CoreHandler>
        class ManagerActor
                : public ServiceActor<_CoreHandler, 3> {
        public:
            bool onInit() override final {
                this->template registerEvent<BestCoreEvent>(*this);
                return true;
            }

            void onEvent(BestCoreEvent &event) {
                event.received();
                event.dest._id = 4;
                event.dest._index = this->getBestCore();
                this->send(event);
            }
        };
    }
}
#endif //CUBE_MANAGERACTOR_H
