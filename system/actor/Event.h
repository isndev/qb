
#ifndef CUBE_EVENT_H
# define CUBE_EVENT_H
# include <bitset>
# include "utils/branch_hints.h"
# include "utils/prefix.h"
# include "utils/timestamp.h"
# include "ActorId.h"

namespace cube {

    template<typename T>
    struct type {
       constexpr static void id() {}
    };

    template<typename T>
    constexpr uint32_t type_id() { return static_cast<uint32_t>(reinterpret_cast<std::size_t>(&type<T>::id)); }
  
    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT Event {
    public:
        uint32_t id;
        uint16_t bucket_size;
        std::bitset<16> state;
        // for users
        ActorId dest;
        ActorId source;

    public:
        Event() = default;
		
        inline ActorId getDestination() const { return dest; }
        inline ActorId getSource() const { return source; }
    };

    struct ServiceEvent : public Event {
      ActorId forward;
      uint32_t service_event_id;
    };

    struct TimedEvent 
      : public ServiceEvent {
      uint64_t start_time;
      uint64_t execution_time;
      
      TimedEvent() = default;
      TimedEvent(Timespan const &span)
      : start_time(Timestamp::nano())
	, execution_time(start_time + span.nanoseconds()) {
	service_event_id = type_id<TimedEvent>();
      }

      inline void onReceive() {
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
      
      IntervalEvent() {
	service_event_id = type_id<IntervalEvent>();
	repeat = ~repeat;
	this->state[0] = 1;
      }
      
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

    struct KillEvent : public Event {
    };

}

#endif //CUBE_EVENT_H
