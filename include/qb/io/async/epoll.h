/**
 * @file qb/io/async/epoll.h
 * @brief Standalone Linux epoll helper (NOT wired into the qb-io event loop).
 *
 * @deprecated This header provides a thin `epoll_create1`/`epoll_ctl`/`epoll_wait`
 *             wrapper that **is not used** by the `qb::io::async::listener`
 *             (which runs on libev). It is retained only as a building block for
 *             users who need direct epoll access outside of the async framework.
 *             New code should prefer the unified `qb::io::async::listener` API.
 *             See `qb/QB_IO_PLAN.md` finding 2.10.
 *
 * This file is only available on Linux systems, as epoll is a Linux-specific API.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB_IO_EPOLL_H
#define QB_IO_EPOLL_H

#ifdef __WIN__SYSTEM__
#error "epoll is not available on windows"
#endif

#include <exception>
#include <qb/utility/branch_hints.h>
#include <sys/epoll.h>
#include "../helper.h"

namespace qb {
namespace io {
namespace epoll {

/**
 * @class Proxy
 * @brief Base class for epoll operations
 *
 * This class provides basic operations for managing epoll file descriptors
 * and controlling the monitored file descriptors.
 */
class Proxy {
protected:
    int _epoll; /**< The epoll file descriptor */

public:
    /**
     * @brief Default constructor
     */
    Proxy() = default;

    /**
     * @brief Constructor
     * @param epoll An existing epoll file descriptor
     */
    Proxy(const int epoll)
        : _epoll(epoll) {}

public:
    /**
     * @brief Type alias for epoll event item
     */
    using item_type = epoll_event;

    /**
     * @brief Copy constructor
     */
    Proxy(Proxy const &) = default;

    /**
     * @brief Modify an existing file descriptor in the epoll set
     *
     * @param item The epoll event item to modify
     * @return 0 on success, -1 on error
     */
    inline int
    ctl(item_type &item) const {
        return epoll_ctl(_epoll, EPOLL_CTL_MOD, item.data.fd, &item);
    }

    /**
     * @brief Add a new file descriptor to the epoll set
     *
     * @param item The epoll event item to add
     * @return 0 on success, -1 on error
     */
    inline int
    add(item_type &item) const {
        return epoll_ctl(_epoll, EPOLL_CTL_ADD, item.data.fd, &item);
    }

    /**
     * @brief Remove a file descriptor from the epoll set
     *
     * @param item The epoll event item to remove
     * @return 0 on success, -1 on error
     */
    inline int
    remove(item_type const &item) {
        return epoll_ctl(_epoll, EPOLL_CTL_DEL, item.data.fd, nullptr);
    }
};

/**
 * @class Poller
 * @brief High-level epoll event poller
 *
 * This template class provides a convenient interface for using epoll to
 * wait for events on multiple file descriptors. It handles the creation
 * and destruction of the epoll file descriptor and provides a simple
 * callback-based interface for event handling.
 *
 * @note Available only on Linux >= 2.6
 * @tparam _MAX_EVENTS Maximum number of events to handle at once
 */
template <std::size_t _MAX_EVENTS = 4096>
class Poller : public Proxy {
    epoll_event _epvts[_MAX_EVENTS]; /**< Buffer for epoll events */

public:
    /**
     * @brief Constructor
     *
     * Creates a new epoll instance with the specified maximum number of events.
     * Throws a runtime_error if the epoll creation fails.
     */
    Poller()
        : Proxy(epoll_create1(EPOLL_CLOEXEC)) {
        if (unlikely(_epoll < 0))
            throw std::runtime_error("failed to init epoll::Poller");
    }

    /**
     * @brief Copy constructor (deleted)
     *
     * Epoll file descriptors should not be shared between objects.
     */
    Poller(Poller const &) = delete;

    /**
     * @brief Destructor
     *
     * Closes the epoll file descriptor.
     */
    ~Poller() {
        ::close(_epoll);
    }

    /**
     * @brief Wait for events and process them
     *
     * This method waits for events on the epoll file descriptor and calls
     * the provided function for each event that occurs.
     *
     * @tparam _Func Type of the callback function
     * @param func Callback function to handle events
     * @param timeout Maximum time to wait in milliseconds (0 = return immediately, -1 =
     * wait indefinitely)
     */
    template <typename _Func>
    inline void
    wait(_Func const &func, int const timeout = 0) {
        const int ret = epoll_wait(_epoll, _epvts, _MAX_EVENTS, timeout);
        if (unlikely(ret < 0)) {
            std::cerr << "epoll::Poller polling has failed " << std::endl;
            return;
        }
        // C++23: Using auto for type deduction
        for (auto i = 0; i < ret; ++i) {
            func(_epvts[i]);
        }
    }
};

} // namespace epoll
} // namespace io
} // namespace qb

#endif // QB_IO_EPOLL_H
