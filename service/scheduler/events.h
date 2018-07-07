
#include "system/actor/Event.h"

#ifndef CUBE_SERVICE_SCHEDULER_EVENTS_H
#define CUBE_SERVICE_SCHEDULER_EVENTS_H

namespace cube {
    namespace service {
        namespace scheduler {
            namespace event {
                // input events
                struct Cancel
                        : public Event {
                    uint64_t time_id;
                };


                // input/output events
                struct Timer
                        : public ServiceEvent {
                    uint64_t time_id;
                    uint64_t start_time;
                    uint64_t execution_time;

                    Timer() = delete;

                    Timer(Timespan const &span)
                            : start_time(Timestamp::nano()), execution_time(start_time + span.nanoseconds()) {
                        service_event_id = type_id<Timer>();
                    }

                    inline void release() {
                        execution_time = 0;
                        this->state[0] = 0;
                    }
                };

                struct Timeout
                        : public Timer {
                    uint32_t repeat;

                    Timeout() = delete;

                    Timeout(Timespan const &span)
                            : Timer(span) {
                        service_event_id = type_id<Timeout>();
                        repeat = ~repeat;
                        this->state[0] = 1;
                    }

                    template<typename _Event, typename _Actor>
                    inline void cancel(_Actor &actor) {
                        state[0] = 0;
                        Cancel e;
                        e.id = type_id<Cancel>();
                        e.time_id = time_id;
                        e.dest = dest;
                        e.source = forward;
                        e.bucket_size = sizeof(Cancel) / CUBE_LOCKFREE_CACHELINE_BYTES;
                        actor.template unregisterEvent<_Event>();
                        actor.reply(e);
                    }

                    void release() {
                        if (likely(static_cast<bool>(--repeat))) {
                            auto now = execution_time;
                            execution_time = now + (now - start_time);
                            start_time = now;
                        } else {
                            Timer::release();
                        }
                    }
                };

            }
        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_EVENTS_H
