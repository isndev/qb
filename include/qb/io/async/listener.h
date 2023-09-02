/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QB_IO_ASYNC_LISTENER_H_
#define QB_IO_ASYNC_LISTENER_H_

#include "event/base.h"
#include <algorithm>
#include <qb/system/container/unordered_set.h>
#include <qb/utility/branch_hints.h>
#include <qb/utility/type_traits.h>
#include <thread>
#include <vector>

namespace qb::io::async {

class listener {
public:
    thread_local static listener current;

    template <typename _Event, typename _Actor>
    class RegisteredKernelEvent final : public IRegisteredKernelEvent {
        friend class listener;

        _Actor &_actor;
        _Event _event;

        ~RegisteredKernelEvent() final = default;

        explicit RegisteredKernelEvent(ev::loop_ref loop, _Actor &actor) noexcept
            : _actor(actor)
            , _event(loop) {}

        void
        invoke() final {
            if constexpr (has_member_func_is_alive<_Actor>::value) {
                if (likely(_actor.is_alive()))
                    _actor.on(_event);
            } else
                _actor.on(_event);
        }
    };

private:
    ev::dynamic_loop _loop;
    qb::unordered_set<IRegisteredKernelEvent *> _registeredEvents;
    std::size_t _nb_invoked_events = 0;

public:
    listener()
        : _loop(EVFLAG_AUTO) {}

    void
    clear() {
        for (auto it : _registeredEvents)
            delete it;
        _registeredEvents.clear();
        run(EVRUN_ONCE);
    }

    ~listener() noexcept {
        clear();
    }

    template <typename EV_EVENT>
    void
    on(EV_EVENT &event, int revents) {
        auto &w = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
        w._revents = revents;
        w._interface->invoke();
        ++_nb_invoked_events;
    }

    template <typename _Event, typename _Actor, typename... _Args>
    _Event &
    registerEvent(_Actor &actor, _Args &&...args) {
        auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
        revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(
            this);
        revent->_event._interface = revent;

        if constexpr (sizeof...(_Args) > 0)
            revent->_event.set(std::forward<_Args>(args)...);

        _registeredEvents.emplace(revent);
        return revent->_event;
    }

    void
    unregisterEvent(IRegisteredKernelEvent *kevent) {
        _registeredEvents.erase(kevent);
        delete kevent;
    }

    [[nodiscard]] inline ev::loop_ref
    loop() const {
        return _loop;
    }

    inline void
    run(int flag = 0) {
        _nb_invoked_events = 0;
        _loop.run(flag);
    }

    [[nodiscard]] inline std::size_t
    nb_invoked_event() const {
        return _nb_invoked_events;
    }

    [[nodiscard]] inline std::size_t
    size() const {
        return _registeredEvents.size();
    }
};

inline void
init() {
    listener::current.clear();
}

inline std::size_t
run(int flag = 0) {
    listener::current.run(flag);
    return listener::current.nb_invoked_event();
}

inline void
run_until(bool const &status) {
    while (status)
        listener::current.run(EVRUN_ONCE);
}

} // namespace qb::io::async

#endif // QB_IO_ASYNC_LISTENER_H_
