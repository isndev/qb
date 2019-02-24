#include            <sys/epoll.h>
#include            <exception>
#include            <cube/utility/branch_hints.h>
#include            "helper.h"

#ifdef __WIN__SYSTEM__
#error "epoll is not available on windows"
#endif

#ifndef             CUBE_NETWORK_EPOLL_H
#define             CUBE_NETWORK_EPOLL_H

namespace           cube {
    namespace       network {
        namespace   epoll {

            class proxy {
            protected:
                int _epoll;
            public:
                proxy() = default;

                proxy(const int epoll)
                        : _epoll(epoll) {
                }

            public:

                using item_type = epoll_event;

                proxy(proxy const &) = default;

                inline int
                ctl(item_type &item) const {
                    return epoll_ctl(_epoll, EPOLL_CTL_MOD, item.data.fd, &item);
                }

                inline int
                add(item_type &item) const {
                    return epoll_ctl(_epoll, EPOLL_CTL_ADD, item.data.fd, &item);
                }

                inline int
                remove(item_type const &item) {
                    return epoll_ctl(_epoll, EPOLL_CTL_DEL, item.data.fd, nullptr);
                }
            };

            template<std::size_t _MAX_EVENTS = 4096>
            class poller
                    : public proxy {
                epoll_event _epvts[_MAX_EVENTS];
            public:
                poller()
                        : proxy(epoll_create(_MAX_EVENTS)) {
                    if (unlikely(_epoll < 0))
                        throw std::runtime_error("failed to init epoll::poller");
                }

                poller(poller const &) = delete;

                ~poller() {
                    ::close(_epoll);
                }

                template<typename _Func>
                inline void
                wait(_Func const &func, int const timeout = 0) {
                    const int ret = epoll_wait(_epoll, _epvts, _MAX_EVENTS, timeout);
                    if (unlikely(ret < 0)) {
                        std::cerr << "epoll::poller polling has failed " << std::endl;
                        return;
                    }
                    for (int i = 0; i < ret; ++i) {
                        func(_epvts[i]);
                    }
                }
            };

        } // namespace epoll
    } // namespace network
} // namespace cube

#endif // CUBE_NETWORK_EPOLL_H
