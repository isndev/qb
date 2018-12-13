
#include "../../actor.h"
#include "events.h"
#include "tags.h"

#ifndef CUBE_SERVICE_SCHEDULER_ACTOR_H
#define CUBE_SERVICE_SCHEDULER_ACTOR_H

namespace cube {
    namespace service {
        namespace scheduler {

            class Actor
                    : public ServiceActor,
                      public Pipe,
                      public ICallback

            {
            public:

                Actor()
                        : ServiceActor(Tag::sid)
                {}

                bool onInit() override final {
                    this->template registerEvent<event::TimedEvent>(*this);
                    this->template registerEvent<event::Cancel>(*this);
                    this->registerCallback(*this);
                    return true;
                }

                void on(event::TimedEvent const &event) {
                    auto &e = this->recycle(event, event.bucket_size);
                    e.time_id = (reinterpret_cast<cube::CacheLine *>(&e)) - this->data();
                    e.received();
                }

                void on(event::Cancel const &event) {
                    auto &e = *reinterpret_cast<event::TimedEvent *>(this->data() + event.getTimeId());
                    e.release();
                }

                virtual void onCallback() override final {
                    uint64_t const now = this->time();

                    if (this->end()) {
                        auto i = this->begin();
                        while (i < this->end()) {
                            auto &event = *reinterpret_cast<event::TimedEvent *>(this->data() + i);
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

        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_ACTOR_H
