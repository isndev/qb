//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "../../actor.h"
#include "events.h"

#ifndef CUBE_SESSION_ACTOR_H
#define CUBE_SESSION_ACTOR_H

namespace cube {
    namespace session {

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

        template <typename Derived>
        class Actor
                : public cube::Actor {

            uint64_t limit_time_activity;
        protected:
            Actor() {}

            inline void reset_timer(std::size_t const seconds) {
                limit_time_activity = this->time() + cube::Timespan::seconds(seconds + 1).nanoseconds();
            }

            inline void repoll(cube::service::iopoll::Proxy &event) const {
                event.setEvents(Derived::type);
                event.repoll();
            }


        public:
            virtual bool onInit() override final {
                this->template registerEvent<event::Ready>(*this);
                return static_cast<Derived &>(*this).onInitialize();
            }

            // Actor input events
            void on(event::Ready &event) {
                if constexpr (Derived::type == Type::WRITE) {

                    if (event.getEvents() & EPOLLOUT
                        && static_cast<Derived &>(*this).onWrite(event))
                        repoll(event);
                    else
                        static_cast<Derived &>(*this).onDisconnect(event);

                } else if constexpr (Derived::type == Type::READ) {

                    if (event.getEvents() & EPOLLIN
                        && static_cast<Derived &>(*this).onRead(event))
                        repoll(event);
                    else
                        static_cast<Derived &>(*this).onDisconnect(event);

                } else {

                    bool status = true;
                    if (event.getEvents() & EPOLLOUT) {
                        // Socket write workflow
                        status = static_cast<Derived &>(*this).onWrite(event);
                    }

                    if (status && event.getEvents() & EPOLLIN)
                        status = static_cast<Derived &>(*this).onRead(event);
                    else if (this->time() > limit_time_activity) {
                        // check activity
                        status = false;
                        LOG_INFO << "Will Disconnect for timer";
                    }
                    else if (status)
                        repoll(event);

                    if (!status)
                        static_cast<Derived &>(*this).onDisconnect(event);
                }
            }

        };
    }
}

#endif //CUBE_SESSION_ACTOR_H
