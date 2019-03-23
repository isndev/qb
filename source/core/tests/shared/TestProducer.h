//
// Created by isnDev on 3/23/2019.
//

#ifndef QB_TESTPRODUCER_H
#define QB_TESTPRODUCER_H

#include <qb/actor.h>
#include "TestLatency.h"

template <typename Event, std::size_t Throughput = 1000>
class ProducerActor
        : public qb::Actor
        , public qb::ICallback {
    const qb::ActorIds _idList;
    uint64_t _max_events;
    uint64_t _timer;
    pg::latency<1000 * 1000, 900000> _latency;

    inline void reset_timer() {
        _timer = time() + Throughput;
    }
public:

    ~ProducerActor() {
        _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }

    ProducerActor(qb::ActorIds const &ids, uint64_t const max)
            : _idList(ids)
            ,  _max_events(max) {
    }

    virtual bool onInit() override final {
        registerEvent<Event>(*this);
        registerCallback(*this);
        reset_timer();
        return true;
    }

    virtual void onCallback() override final {
        if (time() >= _timer) {
            for (auto to : _idList)
                send<Event>(to, id());
            reset_timer();
        }
    }

    void on(Event &event) {
        _latency.add(std::chrono::high_resolution_clock::now() - event._timepoint);
        --_max_events;
        if (!_max_events) {
            unregisterCallback(*this);
            kill();
            broadcast<qb::KillEvent>();
        }
    }
};

#endif //QB_TESTPRODUCER_H
