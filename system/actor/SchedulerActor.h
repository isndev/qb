//
// Created by moham on 6/18/2018.
//

#ifndef CUBE_SCHEDULERACTOR_H
#define CUBE_SCHEDULERACTOR_H

#include <thread>
#include <algorithm>
#include "Actor.h"
#include "Event.h"
#include "../handler/Types.h"
#include "../../utils/timestamp.h"

namespace cube {


	template <typename _CoreHandler, typename _TimedEvent>
	struct TimedPipe : public _CoreHandler::Pipe
	{
		using event_t = _TimedEvent;

		void onEvent(event_t const &event) {
			auto &e = *reinterpret_cast<event_t *>(this->template allocate_async(event.bucket_size));
			memcpy(&e, event, event.bucket_size * CUBE_LOCKFREE_CACHELINE_BYTES);
			e.onReceive();
		};

		template <typename _Actor>
		void execute(_Actor &actor, uint64_t const now) {
			if (this->end()) {
				auto i = this->begin();
				while (i < this->end()) {
					auto &event = *reinterpret_cast<event_t *>(this->_buffer.data() + i);
					bool free_event = false;
					if (!event.execution_time)
						free_event = i == this->begin();
					else if (now >= event.execution_time && actor.send(event)) {
						free_event = i == this->begin();
						event.release();
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

    template<typename _CoreHandler, typename _TComposition>
    class BaseSchedulerActor
      : public ServiceActor<_CoreHandler, 1>,
			  public _TComposition,
              public _CoreHandler::ICallback

    {
    public:
        bool onInit() override final {

			this->each([this](auto &pipe) {
			    using pipe_t = typename std::remove_reference<decltype(pipe)>::type;
			    this->template registerEvent<typename pipe_t::event_t>(*this);
			    return true;
			});

            this-> template registerCallback(*this);
            return true;
        }

        void onCallback() override final {
			uint64_t const now = Timestamp::nano();

			this->each([this, now](auto &pipe){
			    pipe.execute(*this, now);
			    return true;
			});
        }

    };

    template <typename _CoreHandler>
	using SchedulerActor
	= BaseSchedulerActor<_CoreHandler,
			TComposition<TimedPipe<_CoreHandler, TimedEvent>, TimedPipe<_CoreHandler, IntervalEvent>>>;

}
#endif //CUBE_SCHEDULERACTOR_H
