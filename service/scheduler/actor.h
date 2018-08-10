
#include "../../system/actor/Actor.h"
#include "events.h"
#include "tags"

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

                template<typename Handler, typename _SchedEvent, std::size_t _Id>
                class BaseActor
                        : public ServiceActor<Handler, _Id>,
                          public Handler::Pipe,
                          public Handler::ICallback

                {
                public:
                    using event_type = _SchedEvent;

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

            template <typename Handler>
            using ActorTimer = internal::Actor<internal::BaseActor<Handler, event::Timer, Tags<Handler::_index>::uid_timer>>;
            template <typename Handler>
            using ActorTimeout = internal::Actor<internal::BaseActor<Handler, event::Timeout, Tags<Handler::_index>::uid_timeout>>;

        }
    }
}

#endif //CUBE_SERVICE_SCHEDULER_ACTOR_H
