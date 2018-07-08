
#include "../../network/epoll.h"
#include "../../system/actor/Event.h"

#ifndef CUBE_SERVICE_IOPOLL_EVENTS_H
#define CUBE_SERVICE_IOPOLL_EVENTS_H

namespace cube {
    namespace service {
        namespace iopoll {
            namespace event {

                struct Base
                        : public Event {
                    epoll_event ep_event;
                };

                // input events
                struct Subscribe
                        : public Base {
                };

                struct Unsubscribe
                        : public Base {
                };

                // output events
                struct Ready
                        : public Base {
                    network::epoll_proxy proxy;

                    Ready() = delete;

                    Ready(network::epoll_proxy const &proxy, epoll_event const &event)
                            : proxy(proxy) {
                        ep_event = event;
                    }
                };
            }
        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_EVENTS_H
