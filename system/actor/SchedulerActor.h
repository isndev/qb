
#ifndef CUBE_SCHEDULERACTOR_H
# define CUBE_SCHEDULERACTOR_H
# include <thread>
# include <algorithm>

# include "Actor.h"
# include "Event.h"
# include "utils/timestamp.h"

namespace cube {

    struct TimedEvent
            : public ServiceEvent {
        uint64_t start_time;
        uint64_t execution_time;

        TimedEvent() = delete;
        TimedEvent(Timespan const &span)
                : start_time(Timestamp::nano())
                , execution_time(start_time + span.nanoseconds()) {
            service_event_id = type_id<TimedEvent>();
        }

        inline void received() {
            std::swap(dest, forward);
            std::swap(id, service_event_id);
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
                    else if (now >= event.execution_time && this->send(event)) {
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
            this->registerCallback(*this);
            return true;
        }

        void onEvent(event_type const &event) {
            auto &e = this->recycle(event, event.bucket_size);
            e.received();
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
