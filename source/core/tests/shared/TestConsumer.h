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

#ifndef QB_TESTCONSUMER_H
#define QB_TESTCONSUMER_H

#include <qb/actor.h>

template <typename Event>
class ConsumerActor
        : public qb::Actor {
    const qb::ActorIds _idList;
public:

    explicit ConsumerActor(qb::ActorIds const ids = {})
            : _idList(ids)
    {
    }

    virtual bool onInit() override final {
        registerEvent<Event>(*this);
        return true;
    }

    void on(Event &event) {
        if (_idList.size()) {
            for (auto to : _idList)
                send<Event>(to, event);
        } else
            send<Event>(event._ttl, event);
    }
};

#endif //QB_TESTCONSUMER_H
