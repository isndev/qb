/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#include            <sys/epoll.h>
#include            <exception>
#include            <qb/utility/branch_hints.h>
#include            "helper.h"

#ifdef __WIN__SYSTEM__
#error "epoll is not available on windows"
#endif

#ifndef             QB_NETWORK_EPOLL_H
#define             QB_NETWORK_EPOLL_H

namespace           qb {
    namespace       network {
        namespace   epoll {

            class Proxy {
            protected:
                int _epoll;
            public:
                Proxy() = default;

                Proxy(const int epoll)
                        : _epoll(epoll) {
                }

            public:

                using item_type = epoll_event;

                Proxy(Proxy const &) = default;

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

            /*!
             * @class Poller epoll.h qb/network/epoll.h
             * @ingroup Network
             * @note Available only on Linux >= 2.6
             * @tparam _MAX_EVENTS
             */
            template<std::size_t _MAX_EVENTS = 4096>
            class Poller
                    : public Proxy {
                epoll_event _epvts[_MAX_EVENTS];
            public:
                Poller()
                        : Proxy(epoll_create(_MAX_EVENTS)) {
                    if (unlikely(_epoll < 0))
                        throw std::runtime_error("failed to init epoll::Poller");
                }

                Poller(Poller const &) = delete;

                ~Poller() {
                    ::close(_epoll);
                }

                template<typename _Func>
                inline void
                wait(_Func const &func, int const timeout = 0) {
                    const int ret = epoll_wait(_epoll, _epvts, _MAX_EVENTS, timeout);
                    if (unlikely(ret < 0)) {
                        std::cerr << "epoll::Poller polling has failed " << std::endl;
                        return;
                    }
                    for (int i = 0; i < ret; ++i) {
                        func(_epvts[i]);
                    }
                }
            };

        } // namespace epoll
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_EPOLL_H
