

#ifndef QB_TESTPRODUCER_H
#define QB_TESTPRODUCER_H

#include <qb/actor.h>
#include "BenchmarkIterationSink.h"
#include "TestLatency.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

template <typename Event>
class ProducerActor final : public qb::Actor {
    const qb::ActorIdList            _idList;
    uint64_t                         _max_events;
    pg::latency<1000 * 1000, 900000> _latency;
    bool const                       _print_histogram;

public:
    ~ProducerActor() final {
        if (_latency.sample_count()) {
            qb::bench::record_last_latency(_latency.mean_nanoseconds(), static_cast<std::uint64_t>(_latency.sample_count()));
        }
        if (_print_histogram || std::getenv("QB_ACTOR_BENCH_HISTOGRAM")) {
            if (_latency.sample_count())
                _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
        }
    }

    ProducerActor(qb::ActorIdList ids, uint64_t const max, bool print_histogram = false)
        : _idList(std::move(ids))
        , _max_events(max)
        , _print_histogram(print_histogram) {
        registerEvent<Event>(*this);
        for (auto to : _idList)
            send<Event>(to, id());
    }

    void
    on(Event &event) {
        _latency.add(std::chrono::steady_clock::now() - event._timepoint);
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
