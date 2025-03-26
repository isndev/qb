

#ifndef QB_TESTPRODUCER_H
#define QB_TESTPRODUCER_H

#include "TestLatency.h"
#include <qb/actor.h>

#include <utility>

template <typename Event>
class ProducerActor final : public qb::Actor {
    const qb::ActorIdList _idList;
    uint64_t _max_events;
    pg::latency<1000 * 1000, 900000> _latency;

public:
    ~ProducerActor() final {
        _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }

    ProducerActor(qb::ActorIdList ids, uint64_t const max)
        : _idList(std::move(ids))
        , _max_events(max) {
        registerEvent<Event>(*this);
        for (auto to : _idList)
            send<Event>(to, id());
    }

    void
    on(Event &event) {
        _latency.add(std::chrono::high_resolution_clock::now() - event._timepoint);
        --_max_events;
        if (!_max_events) {
            kill();
            broadcast<qb::KillEvent>();
        } else if (!(_max_events % _idList.size())) {
            for (auto to : _idList)
                send<Event>(to, id());
        }
    }
};

#endif // QB_TESTPRODUCER_H
