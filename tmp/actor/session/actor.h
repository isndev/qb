//
// Created by isndev on 7/8/18.
//

#include <algorithm>
#include <vector>

#include "actor.h"
#include "service/iopoll/routine.h"
#include "session/types.h"
#include "events.h"

#ifndef QB_SESSION_ACTOR_H
#define QB_SESSION_ACTOR_H

namespace qb {
    namespace session {

        template <typename Derived>
        class Actor
                : public service::iopoll::Routine<Derived>
                , public qb::Actor {
        protected:
            Actor() {}

            inline void reset_timer(std::size_t const seconds) {
                this->setTimer(this->time() + static_cast<uint64_t>(qb::Timespan::seconds(seconds + (seconds / 2)).nanoseconds()));
            }

        public:
            virtual bool onInit() override final {
                registerEvent<event::Ready>(*this);
                return static_cast<service::iopoll::Routine<Derived> &>(*this).onInitialize();
            }

            // Actor input events
            void on(event::Ready &event) {
                static_cast<service::iopoll::Routine<Derived> &>(*this).on(event);
            }

        };
    }
}

#endif //QB_SESSION_ACTOR_H
