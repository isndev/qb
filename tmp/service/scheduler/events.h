
#include "../../system/actor/Event.h"

#ifndef CUBE_SERVICE_SCHEDULER_EVENTS_H
#define CUBE_SERVICE_SCHEDULER_EVENTS_H

namespace cube {
    namespace service {
        namespace scheduler {

            class Actor;

            namespace event {
                // input events
                class Cancel
                        : public Event {
                    uint64_t time_id;
                public:
                    Cancel() = delete;
                    Cancel(uint64_t const id)
                            : time_id(id) {}

                    uint64_t getTimeId() const {
                        return time_id;
                    }

                };

                // input/output events
                struct TimedEvent
                        : public ServiceEvent {
                    friend class scheduler::Actor;
                    uint64_t time_id;
                    uint64_t start_time;
                    uint64_t execution_time;
                public:
                    uint32_t repeat;

                    TimedEvent() = delete;

                    TimedEvent(Timespan const &span, uint32_t const repeat = 1)
                            : start_time(Timestamp::nano())
                            , execution_time(start_time + span.nanoseconds())
                            , repeat(repeat)
                    {
                        service_event_id = type_id<TimedEvent>();
                        this->state[0] = 1;
                    }

                    template<typename _Event, typename _Actor>
                    inline void cancel(_Actor &actor) {
                        state[0] = 0;
                        actor.template unregisterEvent<_Event>();
                        actor.template send<Cancel>(forward, time_id);
                    }

                    void release() {
                        if (likely(static_cast<bool>(--repeat))) {
                            auto now = execution_time;
                            execution_time = now + (now - start_time);
                            start_time = now;
                        } else {
                            execution_time = 0;
                            this->state[0] = 0;
                        }
                    }
                };

            }
        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_EVENTS_H
