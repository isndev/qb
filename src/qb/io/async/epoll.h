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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

// Must precede the guard below: __WIN__SYSTEM__ is qb's own macro, defined here and
// nowhere else. It used to arrive through "../helper.h"; while that include was dangling
// the #error could never fire, so the "not available on windows" guard was inert.
#include <qb/utility/build_macros.h>

#ifdef __WIN__SYSTEM__
#error "epoll is not available on windows"
#endif

// `qb/io/helper.h` was deleted in 581094a9 ("change qb::io lowlevel abstraction") when the
// socket layer moved to qb/io/system/sys__socket.h; this header kept the include and has
// been unbuildable ever since -- an installed public header (see install_manifest) that no
// consumer could compile. It is restored as the exact set of headers it was relied on for:
// POSIX close() (<unistd.h>), std::cerr (<iostream>), and the build macros above. The
// enum SocketType/SocketStatus it also carried are NOT used here; qb::io::SocketStatus now
// lives in qb/io/system/sys__socket.h and is deliberately not pulled in -- this header is
// standalone by design.
#include <cstddef>
#include <exception>
#include <iostream>
#include <qb/utility/branch_hints.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

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
        // Modern C++: using auto for type deduction
        for (auto i = 0; i < ret; ++i) {
            func(_epvts[i]);
        }
    }
};

} // namespace epoll
} // namespace io
} // namespace qb

#endif // QB_IO_EPOLL_H
