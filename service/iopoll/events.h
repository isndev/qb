
#include "../../network/epoll.h"
#include "../../network/SocketTCP.h"
#include "../../network/SocketUDP.h"
#include "../../system/actor/Event.h"

#ifndef CUBE_SERVICE_IOPOLL_EVENTS_H
#define CUBE_SERVICE_IOPOLL_EVENTS_H

namespace cube {
    namespace service {
        namespace iopoll {

	  struct handle_proxy {
	    epoll_event ep_event;
	    
	    network::SocketUDP const udp() const { return *(reinterpret_cast<const network::SocketUDP*>(&ep_event.events + 1)); }
	    network::SocketTCP const &tcp() const { return *reinterpret_cast<const network::SocketTCP*>(&ep_event.events + 1); }
	    inline void setHandle(int const fd) { ep_event.data.fd = fd; }
	    inline void setOwner(uint32_t const id) { *(&ep_event.data.u32 + 1) = id; }
	    inline void setEvents(uint32_t const events) { ep_event.events = events; }
	    
	    inline int getHandle() const { return ep_event.data.fd; }
	    inline uint32_t getOwner() const { return *(&ep_event.data.u32 + 1); }
	    inline uint32_t getEvents() const { return ep_event.events; }
	    
	  };

	  struct Proxy
	    : public handle_proxy {
	    const network::epoll_proxy proxy;
	    Proxy() = delete;
	    Proxy(Proxy const &) = default;
	    Proxy(network::epoll_proxy const &proxy)
	      : proxy(proxy) {}
	    
	    inline int repoll() {
	      ep_event.events |= EPOLLONESHOT;
	      return proxy.ctl(ep_event);
	    }
	  };
	  
            namespace event {
	      
	      struct Base : public Event
		          , public handle_proxy {
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
		  : public Event
		  , public Proxy {

		Ready() = delete;
		Ready(network::epoll_proxy const &proxy, epoll_event const &event)
		  : Proxy(proxy) {
                        ep_event = event;
		  }
                };
            }
        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_EVENTS_H
