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


    template<typename _CoreHandler, typename _Trait>
    class SchedulerActor
            : public Actor<_CoreHandler>,
              public _CoreHandler::ICallback

    {
        typename _CoreHandler::Pipe pipe;
    public:
        bool onInit() override final {
            this-> template registerEvent<TimedEvent>(*this);
            this-> template registerCallback(*this);
            return true;
        }

        void onCallback() override final {
            uint64_t const now = Timestamp::nano();

            if (pipe.end()) {
                auto i = pipe.begin();
                while (i < pipe.end()) {
                    auto &event = *reinterpret_cast<TimedEvent *>(pipe.buffer() + i);
                    bool free_event = false;
                    if (!event.duration)
                        free_event = i == pipe.begin();
                    else if (now >= event.duration && this->send(event)) {
                        free_event = i == pipe.begin();
                        event.duration = 0;
                    }
                    if (free_event)
                        pipe.free_front(event.bucket_size);
                    i += event.bucket_size;
                }
                if (pipe.begin() == pipe.end())
                    pipe.reset();
            }

            //std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        void onEvent(TimedEvent const &event) {
            auto &e = pipe.template dynallocate<TimedEvent>(reinterpret_cast<CacheLine const *>(&event), event.bucket_size);
            const_cast<TimedEvent &>(event).state[0] = 1;
            std::swap(e.dest, e.source);
            std::swap(e.id, e.save_id);
        }
    };

}
#endif //CUBE_SCHEDULERACTOR_H
