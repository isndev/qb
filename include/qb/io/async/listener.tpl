//
// Created by isnDev on 9/17/2019.
//

#ifndef QB_IO_ASYNC_LISTENER_TPL
#define QB_IO_ASYNC_LISTENER_TPL

namespace qb {
    namespace io {
        namespace async {

//            thread_local struct ev::dynamic_loop Default::loop = { EVFLAG_AUTO };
//            thread_local listener listener::current;

//            thread_local listener listener::current;

//            template<typename EV_EVENT>
//            void listener::on(EV_EVENT &event, int revents) {
//                auto &w = *reinterpret_cast<event::base<EV_EVENT> *>(&event);
//                w._revents = revents;
//                w._interface->invoke();
//            }
//
//            template<typename _Event, typename _Actor, typename ..._Args>
//            _Event &listener::registerEvent(_Actor &actor, _Args ...args) {
//                auto revent = new RegisteredKernelEvent<_Event, _Actor>(_loop, actor);
//                revent->_event.template set<listener, &listener::on<typename _Event::ev_t>>(this);
//                revent->_event._interface = revent;
//
//                if constexpr (sizeof...(_Args) > 0)
//                    revent->_event.set(std::forward<_Args>(args)...);
//
//                _registeredEvents.push_back(revent);
//                std::cout << _registeredEvents.size() << " registered events" << std::endl;
//                return revent->_event;
//            }

        }
    }
}

#endif //QB_IO_ASYNC_LISTENER_TPL
