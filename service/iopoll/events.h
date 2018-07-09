
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

                    inline void setHandle(int const fd) { ep_event.data.fd = fd; }
                    inline void setOwner(uint32_t const id) { *(&ep_event.data.u32 + 1) = id; }
                    inline void setEvents(uint32_t const events) { ep_event.events = events; }

                    inline int getHandle() const { return ep_event.data.fd; }
                    inline uint32_t getOwner() const { return *(&ep_event.data.u32 + 1); }
                    inline uint32_t getEvents() const { return ep_event.events; }

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
                    const network::epoll_proxy proxy;

                    Ready() = delete;

                    Ready(network::epoll_proxy const &proxy, epoll_event const &event)
                            : proxy(proxy) {
                        ep_event = event;
                    }

                    inline int repoll() {
                        ep_event.events |= EPOLLONESHOT;
                        return proxy.ctl(ep_event);
                    }
                };
            }
        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_EVENTS_H
