
#include "../../system/actor/Actor.h"
#include "events.h"
#include "tags.h"

#ifndef CUBE_SERVICE_MANAGER_ACTOR_H
#define CUBE_SERVICE_MANAGER_ACTOR_H

namespace cube {
    namespace service {
        namespace manager {

            template<typename Handler>
            class ActorAgent
                    : public ServiceActor<Handler, Tags<Handler::_index>::uid_agent> {
            };

            template<typename Handler>
            class Actor
                    : public ServiceActor<Handler, Tags<Handler::_index>::uid> {

                inline void received(event::Base &event) {
                    event.received();
                    if (event.dest == event.source)
                        event.dest._id = 4;
                }

            public:
                virtual bool onInit() override final {
                    this->template registerEvent<event::ToBestTimedCore>(*this);
                    this->template registerEvent<event::ToCore>(*this);
                    return true;
                }

                void on(event::ToBestTimedCore &event) {
                    received(event);

                    event.dest._index = this->bestCore();
                    this->send(event);
                }

                void on(event::ToCore &event) {
                    received(event);

                    event.dest._index = event.index;
                    this->send(event);
                }
            };

        }
    }
}

#endif //CUBE_SERVICE_MANAGER_ACTOR_H
