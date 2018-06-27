
#ifndef CUBE_SCHEDULERACTOR_H
# define CUBE_SCHEDULERACTOR_H
# include <thread>
# include <algorithm>

# include "utils/timestamp.h"
# include "system/actor/Actor.h"
# include "system/actor/Event.h"

namespace cube {

    struct CancelTimedEvent
            : public Event {
        uint64_t time_id;
    };

    struct TimedEvent
            : public ServiceEvent {
        uint64_t time_id;
        uint64_t start_time;
        uint64_t execution_time;

        TimedEvent() = delete;
        TimedEvent(Timespan const &span)
                : start_time(Timestamp::nano())
                , execution_time(start_time + span.nanoseconds()) {
            service_event_id = type_id<TimedEvent>();
        }

        inline void release() {
            execution_time = 0;
            this->state[0] = 0;
        }
    };

    struct IntervalEvent
            : public TimedEvent {
        uint32_t repeat;

        IntervalEvent() = delete;

        IntervalEvent(Timespan const &span)
                : TimedEvent(span) {
            service_event_id = type_id<IntervalEvent>();
            repeat = ~repeat;
            this->state[0] = 1;
        }

        template <typename _Event, typename _Actor>
        inline void cancel(_Actor &actor) {
            state[0] = 0;
            CancelTimedEvent e;
            e.id = type_id<CancelTimedEvent>();
            e.time_id = time_id;
            e.dest = dest;
            e.source = forward;
            e.bucket_size = sizeof(CancelTimedEvent) / CUBE_LOCKFREE_CACHELINE_BYTES;
            actor.template unregisterEvent<_Event>();
            actor.reply(e);
        }

        void release() {
            if (likely(static_cast<bool>(--repeat))) {
                auto now = execution_time;
                execution_time = now + (now - start_time);
                start_time = now;
            } else {
                TimedEvent::release();
            }
        }
    };

    template <typename _BaseActor>
    class SchedulerActor : public _BaseActor
    {
    public:
        using event_type = typename _BaseActor::event_type;

        virtual void onCallback() override final {
            uint64_t const now = Timestamp::nano();

            if (this->end()) {
                auto i = this->begin();
                while (i < this->end()) {
                    auto &event = *reinterpret_cast<event_type *>(this->data() + i);
                    bool free_event = false;
                    if (!event.execution_time)
                        free_event = i == this->begin();
                    else if (now >= event.execution_time && this->try_send(event)) {
                        event.release();
                        free_event = (!event.execution_time && i == this->begin());
                    }
                    if (free_event)
                        this->free_front(event.bucket_size);
                    i += event.bucket_size;
                }
                if (this->begin() == this->end())
                    this->reset();
            }
        }
    };

    template<typename _CoreHandler, typename _SchedEvent, std::size_t _Id>
    class BaseSchedulerActor
            : public ServiceActor<_CoreHandler, _Id>,
              public _CoreHandler::Pipe,
              public _CoreHandler::ICallback

    {
    public:
        using event_type = _SchedEvent;

        bool onInit() override final {
            this->template registerEvent<_SchedEvent>(*this);
            if constexpr (std::is_same<_SchedEvent, IntervalEvent>::value)
                this->template registerEvent<CancelTimedEvent>(*this);
            this->registerCallback(*this);
            return true;
        }

        void on(event_type const &event) {
            auto &e = this->recycle(event, event.bucket_size);
            e.time_id = (reinterpret_cast<cube::CacheLine *>(&e)) - this->data();
            e.received();
        }

        void on(CancelTimedEvent const &event) {
            auto &e = *reinterpret_cast<event_type *>(this->data() + event.time_id);
            e.release();
        }

    };

    namespace service
    {
        template <typename CoreHandler>
        using TimerActor = SchedulerActor<BaseSchedulerActor<CoreHandler, TimedEvent, 1>>;
        template <typename CoreHandler>
        using IntervalActor = SchedulerActor<BaseSchedulerActor<CoreHandler, IntervalEvent, 2>>;
    }

}

#endif //CUBE_SCHEDULERACTOR_H
