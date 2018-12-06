
#include "../../actor.h"
#include "events.h"
#include "tags.h"

#ifndef CUBE_SERVICE_SCHEDULER_ACTOR_H
#define CUBE_SERVICE_SCHEDULER_ACTOR_H

namespace cube {
    namespace service {
        namespace scheduler {

            namespace internal {
                template <typename _BaseActor>
                class Actor : public _BaseActor
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

                template <typename T>
                constexpr uint16_t resolve_default() {
                    if constexpr (std::is_same<T, event::Timeout>::value)
                        return TimeoutTag::sid;
                    else
                        return TimerTag::sid;
                }

                template<typename _SchedEvent>
                class BaseActor
                        : public ServiceActor,
                          public Core::Pipe,
                          public ICallback

                {
                public:
                    using event_type = _SchedEvent;

                    BaseActor()
                            : ServiceActor(resolve_default<_SchedEvent>())
                    {}

                    bool onInit() override final {
                        this->template registerEvent<_SchedEvent>(*this);
                        if constexpr (std::is_same<_SchedEvent, event::Timeout>::value)
                            this->template registerEvent<event::Cancel>(*this);
                        this->registerCallback(*this);
                        return true;
                    }

                    void on(event_type const &event) {
                        auto &e = this->recycle(event, event.bucket_size);
                        e.time_id = (reinterpret_cast<cube::CacheLine *>(&e)) - this->data();
                        e.received();
                    }

                    void on(event::Cancel const &event) {
                        auto &e = *reinterpret_cast<event_type *>(this->data() + event.time_id);
                        e.release();
                    }

                };
            }

            using ActorTimer = internal::Actor<internal::BaseActor<event::Timer>>;
            using ActorTimeout = internal::Actor<internal::BaseActor<event::Timeout>>;

        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_ACTOR_H
