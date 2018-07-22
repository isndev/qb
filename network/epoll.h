#include            <sys/epoll.h>
#include            <exception>
#include            "../utils/branch_hints.h"
#include            "Socket.h"

#ifndef             CUBE_NETWORK_EPOLL_H
#define             CUBE_NETWORK_EPOLL_H

namespace           cube {
    namespace       network {

        class       epoll_proxy {
        protected:
            int _epoll;
        public:
            epoll_proxy() = default;
            epoll_proxy(const int epoll)
                    : _epoll(epoll) {
            }

        public:

            using item_type = epoll_event;

            epoll_proxy(epoll_proxy const &) = default;

            inline int
            ctl(item_type &item) const {
                return epoll_ctl(_epoll, EPOLL_CTL_MOD, item.data.fd, &item);
            }
        };

        template <std::size_t _MAX_EVENTS = 1024>
        class          epoll
                : public epoll_proxy {
            epoll_event _epvts[_MAX_EVENTS];
        public:
            epoll() throw()
                    : epoll_proxy(epoll_create(_MAX_EVENTS)) {
                if (unlikely(_epoll < 0))
                    throw std::runtime_error("failed to init epoll");
            }

            epoll(epoll const &) = delete;

            ~epoll() {
                ::close(_epoll);
            }

            inline int
            add(item_type &item) const {
                return epoll_ctl(_epoll, EPOLL_CTL_ADD, item.data.fd, &item);
            }

            inline int
            remove(item_type const &item) {
                return epoll_ctl(_epoll, EPOLL_CTL_DEL, item.data.fd, nullptr);
            }

            template<typename _Func>
            inline void
            wait(_Func const &func, int const timeout = 0) {
                const int ret = epoll_wait(_epoll, _epvts, _MAX_EVENTS, timeout);
                if (unlikely(ret < 0)) {
                    std::cerr << "epoll polling has failed " << std::endl;
                    return;
                }
                for (int i = 0; i < ret; ++i) {
                    func(_epvts[i]);
                }
            }
        };

    } //namespace network
} //namespace cube

#endif //CUBE_NETWORK_EPOLL_H
