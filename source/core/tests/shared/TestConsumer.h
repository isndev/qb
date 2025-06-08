

#ifndef QB_TESTCONSUMER_H
#define QB_TESTCONSUMER_H

#include <qb/actor.h>

#include <utility>

template <typename Event>
class ConsumerActor : public qb::Actor {
    const qb::ActorIdList _idList;

public:
    explicit ConsumerActor(qb::ActorIdList ids = {})
        : _idList(std::move(ids)) {
        registerEvent<Event>(*this);
    }

    void
    on(Event &event) {
        if (_idList.size()) {
            for (auto to : _idList)
                send<Event>(to, event);
        } else
            send<Event>(event._ttl, event);
    }
};

#endif // QB_TESTCONSUMER_H
