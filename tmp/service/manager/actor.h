
#include "actor.h"
#include "events.h"
#include "tags.h"

#ifndef QB_SERVICE_MANAGER_ACTOR_H
#define QB_SERVICE_MANAGER_ACTOR_H

namespace qb {
    namespace service {
        namespace manager {

            class ActorAgent
                    : public ServiceActor {
            public:

                ActorAgent()
                : ServiceActor(AgentTag::sid)
                        {}
            };

            class Actor
                    : public ServiceActor {

                inline void received(event::Base &event) {
                    event.received();
                    if (event.dest == event.source)
                        event.dest._id = AgentTag::sid;
                }

            public:

                Actor()
                    : ServiceActor(Tag::sid)
                {}

                virtual bool onInit() override final {
                    registerEvent<event::ToCore>(*this);
                    registerEvent<event::ToCoreRange>(*this);
                    return true;
                }

                void on(event::ToCore &event) {
                    received(event);

                    event.dest._index = event.index;
                    this->push(event);
                }

                void on(event::ToCoreRange &event) {
                    received(event);

                    for (auto i = event.begin; i < event.end; ++i) {
                        event.dest._index = i;
                        this->push(event);
                    }
                }
            };

        }
    }
}

#endif //QB_SERVICE_MANAGER_ACTOR_H
