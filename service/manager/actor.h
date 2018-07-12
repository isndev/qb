
#include "../../system/actor/Actor.h"
#include "events.h"

#ifndef CUBE_SERVICE_MANAGER_ACTOR_H
#define CUBE_SERVICE_MANAGER_ACTOR_H

namespace cube {
    namespace service {
        namespace manager {

            template<typename Handler>
            class ActorAgent
                    : public ServiceActor<Handler, 4> {
            };

            template<typename Handler>
            class Actor
                    : public ServiceActor<Handler, 3> {
            public:
                bool onInit() override final {
                    this->template registerEvent<event::ToBestTimedCore>(*this);
                    this->template registerEvent<event::ToCore>(*this);
                    return true;
                }

                void on(event::ToBestTimedCore &event) {
                    event.received();
                    event.dest._id = 4;
                    event.dest._index = this->bestCore();
                    this->send(event);
                }

                void on(event::ToCore &event) {
                    event.received();
                    event.dest._id = 4;
                    event.dest._index = event.coreIndex;
                    this->send(event);
                }
            };

        }
    }
}

#endif //CUBE_SERVICE_MANAGER_ACTOR_H
