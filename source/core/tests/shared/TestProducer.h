/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#ifndef QB_TESTPRODUCER_H
#define QB_TESTPRODUCER_H

#include <qb/actor.h>
#include "TestLatency.h"

template <typename Event>
class ProducerActor
        : public qb::Actor
{
    const qb::ActorIds _idList;
    uint64_t _max_events;
    pg::latency<1000 * 1000, 900000> _latency;

public:

    ~ProducerActor() {
        _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }

    ProducerActor(qb::ActorIds const ids, uint64_t const max)
            : _idList(ids)
            , _max_events(max)
    {
    }

    virtual bool onInit() override final {
        registerEvent<Event>(*this);
        for (auto to : _idList)
            send<Event>(to, id());
        return true;
    }

    void on(Event &event) {
        _latency.add(std::chrono::high_resolution_clock::now() - event._timepoint);
        --_max_events;
        if (!_max_events) {
            kill();
            broadcast<qb::KillEvent>();
        } else if (!(_max_events % _idList.size())){
            for (auto to : _idList)
                send<Event>(to, id());
        }
    }
};

#endif //QB_TESTPRODUCER_H
