//
// Created by isnDev on 3/23/2019.
//

#ifndef QB_TESTCONSUMER_H
#define QB_TESTCONSUMER_H

#include <qb/actor.h>

template <typename Event>
class ConsumerActor
        : public qb::Actor {
    const qb::ActorIds _idList;
public:

    explicit ConsumerActor(qb::ActorIds const ids)
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
