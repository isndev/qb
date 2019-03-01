
#include "actor.h"
#include "events.h"
#include "tags.h"

#ifndef CUBE_SERVICE_IOPOLL_ACTOR_H
#define CUBE_SERVICE_IOPOLL_ACTOR_H

namespace cube {
    namespace service {
        namespace iopoll {

            class Actor
                    : public cube::ServiceActor
                    , public cube::ICallback
            {
                network::epoll<> _epoll;
            public:
                Actor()
                    : ServiceActor(Tag::sid) {}

                bool onInit() override final {
                    registerEvent<event::Subscribe>(*this);
                    this->registerCallback(*this);

                    return true;
                }

                void onCallback() override final {
                    _epoll.wait([this](auto &event){
                        push<event::Ready>(*(&event.data.u32 + 1), _epoll, event);
                    });
                }

                void on(event::Subscribe &event) {
                    if (!event.getOwner())
                        event.setOwner(event.source);
                    _epoll.add(event.ep_event);
                }
            };

        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_ACTOR_H
