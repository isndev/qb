
#include "system/actor/Actor.h"
#include "events.h"

#ifndef CUBE_SERVICE_IOPOLL_ACTOR_H
#define CUBE_SERVICE_IOPOLL_ACTOR_H

namespace cube {
    namespace service {
        namespace iopoll {

            template <typename Handler>
            class Actor
                    : public cube::ServiceActor<Handler, 8>
                    , public Handler::ICallback
            {
                network::epoll<> _epoll;
            public:
                Actor() = default;

                bool onInit() override final {
                    this->template registerEvent<event::Subscribe>(*this);
                    this->template registerEvent<event::Unsubscribe>(*this);
                    this->registerCallback(*this);

                    return true;
                }

                void onCallback() override final {
                    _epoll.wait([this](auto &event){
                        this->template send<event::Ready>(*(&event.data.u32 + 1), _epoll, event);
                    });
                }

                void on(event::Subscribe &event) {
                    *(&event.ep_event.data.u32 + 1) = event.source;
                    _epoll.add(event.ep_event);
                }

                void on(event::Unsubscribe const &event) {
                    _epoll.remove(event.ep_event);
                }
            };

        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_ACTOR_H
