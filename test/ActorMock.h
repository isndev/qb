
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

struct TinyEvent : cube::Event {
    uint64_t x;
    TinyEvent(uint64_t x) : x(x) {}
};

struct BigEvent : cube::Event {
    uint64_t x;
    uint64_t padding[127];
    BigEvent(uint64_t x) : x(x) {}
};

struct DynamicEvent : cube::Event {
    uint64_t x;
	std::vector<int> vec;
    DynamicEvent(uint64_t x) : x(x), vec(512, 8) {}
};

struct SharedData {
    std::vector<int> _shared_vec;

    SharedData() : _shared_vec(512, 128){}
};


// TODO: NEED ABSOLUTELY TO AVOID THIS TEMPLATE PARAMETER WITH A PROXY
template<typename Handler = void>
class ActorMock_Tiny : public cube::Actor<Handler> {
    const cube::ActorId actor_to_send;
    int alive = 0;
public:
	ActorMock_Tiny(cube::ActorId const &id = cube::ActorId::NotFound{})
            : actor_to_send(id) {}

    int init() {
        this->template registerEvent<TinyEvent>(*this);
		if (actor_to_send)
			this->template push<TinyEvent>(actor_to_send, 0);
        return 0;
    }

    int main() {
        return alive;
    }

    void onEvent(TinyEvent const &event) {
        if (event.x >= 3000) alive = 1;
        auto &rep = this->template reply<TinyEvent>(event);
        ++rep.x;
    }

};

template<typename Handler = void>
class ActorMock_Big : public cube::Actor<Handler> {
	const cube::ActorId actor_to_send;
	int alive = 0;
public:
	ActorMock_Big(cube::ActorId const &id = cube::ActorId::NotFound{})
		: actor_to_send(id) {}

	int init() {
		this->template registerEvent<BigEvent>(*this);
		if (actor_to_send)
			this->template push<BigEvent>(actor_to_send, 0);
		return 0;
	}

	int main() {
		return alive;
	}

	void onEvent(BigEvent const &event) {
		if (event.x >= 3000) alive = 1;
		auto &rep = this->template reply<BigEvent>(event);
		++rep.x;
	}

};

template<typename Handler = void>
class ActorMock_Dynamic : public cube::Actor<Handler> {
	const cube::ActorId actor_to_send;
	int alive = 0;
public:
	ActorMock_Dynamic(cube::ActorId const &id = cube::ActorId::NotFound{})
		: actor_to_send(id) {}

	int init() {
		this->template registerEvent<DynamicEvent>(*this);
		if (actor_to_send)
			this->template push<DynamicEvent>(actor_to_send, 0);
		return 0;
	}

	int main() {
		return alive;
	}

	void onEvent(DynamicEvent const &event) {
		if (event.x >= 3000) alive = 1;
		auto &rep = this->template reply<DynamicEvent>(event);
		++rep.x;
	}

};


#endif //CUBE_TEST_ACTORMOCK_H
