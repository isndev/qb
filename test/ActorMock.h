
#ifndef CUBE_TEST_ACTORMOCK_H
#define CUBE_TEST_ACTORMOCK_H

#include <iostream>
#include <algorithm>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <limits>

#include "system/actor/PhysicalCore.h"
#include "system/actor/Actor.h"


struct EventMock : cube::Event {
    uint32_t x;

    EventMock(uint32_t x) : x(x) {}
};

// TODO: NEED ABSOLUTELY TO AVOID THIS TEMPLATE PARAMETER WITH A PROXY
template<typename Handler>
class ActorMock : public cube::Actor<Handler> {
    const cube::ActorId actor_to_send;
    bool flag = false;
    int alive = 0;
public:
    ActorMock(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    virtual ~ActorMock() {
        LOG_INFO << "Actor." << this->id() << " destroyed";
    }

    int init() {
        this->template registerEvent<EventMock>(*this);
        LOG_INFO << "Actor." << this->id() << " init";
        return 0;
    }

    int main() {
        //this-> template addRefActor<ActorMock>();
        if (actor_to_send && !flag) {
            flag = true;
            this->template push<EventMock>(actor_to_send, 0);
            LOG_INFO << "Actor." << this->id() << " send " << 0 << " to " << actor_to_send;
        }
        return alive;
    }

    void onEvent(EventMock const &event) {
        // reply
        if (event.x >= 3000)
            alive = 1;
        LOG_INFO << "Actor." << this->id() << " Received event" << " reply " << event.x + 1 << " to " << event.source;
        this->template push<EventMock>(event.source, event.x + 1);
    }

};

#endif //CUBE_TEST_ACTORMOCK_H
