//
// Created by isndev on 12/15/18.
//

#include "../../session/types.h"
#include "events.h"

#ifndef CUBE_SERVICE_IOPOLL_ROUTINE_H
#define CUBE_SERVICE_IOPOLL_ROUTINE_H

namespace cube {
    namespace service {
        namespace iopoll {

            enum Type : uint32_t {
                READ = EPOLLIN,
                WRITE = EPOLLOUT,
                READWRITE = EPOLLIN | EPOLLOUT
            };

            // example trait to implement
            struct ExampleTrait {
                constexpr static const Type type = Type::READWRITE;
                constexpr static bool hasKeepAlive = true;

                bool onInitialize();
                bool onWrite(event::Ready &event);
                bool onRead(event::Ready &event);
                void onDisconnect(event::Ready &event);
            };

            template<typename Derived>
            class Routine {

                uint64_t limit_time_activity;
            protected:
                Routine() : limit_time_activity(0) {}

                inline void setTimer(std::size_t const timer) {
                    limit_time_activity = timer;
                }

                inline void repoll(service::iopoll::Proxy &event) const {
                    event.setEvents(Derived::type);
                    event.repoll();
                }


            public:
                bool onInitialize() {
                    return static_cast<Derived &>(*this).onInitialize();
                }

                // Actor input events
                void on(service::iopoll::Proxy &event) {
                    auto status = session::ReturnValue::KO;

                    if constexpr (Derived::type == Type::WRITE) {
                        status = event.getEvents() & EPOLLOUT
                                 ? static_cast<Derived &>(*this).onWrite(event)
                                 : session::ReturnValue::KO;
                    } else if constexpr (Derived::type == Type::READ) {
                        status = event.getEvents() & EPOLLIN
                                 ? static_cast<Derived &>(*this).onRead(event)
                                 : session::ReturnValue::KO;
                    } else {
                        if (event.getEvents() & EPOLLOUT) {
                            // Socket write workflow
                            status = static_cast<Derived &>(*this).onWrite(event);
                        }
                        if (event.getEvents() & EPOLLIN)
                            status = static_cast<Derived &>(*this).onRead(event);
                    }


                    if constexpr (Derived::has_keepalive) {
                        if (static_cast<Derived &>(*this).time() > limit_time_activity) {
                            // check activity
                            status = session::ReturnValue::KO;
                            LOG_INFO << "Will Disconnect for timer"
                                     << static_cast<Derived &>(*this).time()
                                     << ">" << limit_time_activity
                                     << "DIFF= " << static_cast<Derived &>(*this).time() - limit_time_activity;
                        }
                    }

                    switch (status) {
                        case session::ReturnValue::REPOLL:
                            repoll(event);
                            break;
                        case session::ReturnValue::KO:
                            static_cast<Derived &>(*this).onDisconnect(event);
                            break;
                        default:
                            break;
                    };

                }

            };

        }
    }
}

#endif //CUBE_SERVICE_IOPOLL_ROUTINE_H
