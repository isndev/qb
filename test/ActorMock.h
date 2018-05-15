
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
    ActorMock(cube::ActorId const &id = cube::ActorId::NotFound)
            : actor_to_send(id) {}

    virtual ~ActorMock() {
        cube::io::cout() << *this << " destroyed" << std::endl;
    }

    int init() {
        this->template registerEvent<EventMock>(*this);
        cube::io::cout() << *this << " init" << std::endl;
        return 0;
    }

    int main() {
        //this-> template addRefActor<ActorMock>();
        if (actor_to_send && !flag) {
            flag = true;
            this->template push<EventMock>(actor_to_send, 0);
#ifndef NDEBUG
            cube::io::cout() << *this << " send " << 0 << " to " << actor_to_send << std::endl;
#endif
        }
        return alive;
    }

    void onEvent(EventMock const &event) {
        // reply
        if (event.x >= 3000)
            alive = 1;
#ifndef NDEBUG
        cube::io::cout() << *this << " Received event" << std::endl
                         << *this << " reply " << event.x + 1 << " to " << event.source << std::endl;
#endif
        this->template push<EventMock>(event.source, event.x + 1);
    }

};

#endif //CUBE_TEST_ACTORMOCK_H
