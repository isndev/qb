// PingPongActor.h file
#include <cube/actor.h>
#include "MyEvent.h"
#ifndef PINGPONGACTOR_H_
# define PINGPONGACTOR_H_

class PingPongActor
        : public qb::Actor // /!\ should inherit from cube actor
{
    const qb::ActorId _id_pong; // Pong ActorId
public:
    PingPongActor(const qb::ActorId id_pong = {})
            : _id_pong(id_pong) {}

    // /!\ never call any qb::Actor functions in constructor
    // /!\ use onInit function
    // /!\ the engine will call this function before adding PingPongActor
    bool onInit() override final {
        registerEvent<MyEvent>(*this);             // will listen MyEvent
        if (_id_pong) {                            // is Ping Actor
            auto &event = push<MyEvent>(_id_pong); // push MyEvent to Pong Actor and keep a reference to the event
            event.data = 1337;                     // set trivial data
            event.container.push_back(7331);       // set dynamic data
        }
        return true;                               // init ok
    }
    // will call this function when PingPongActor receives MyEvent
    void on(MyEvent &event) {
        // print some data
        qb::io::cout() << "Actor id(" << id() << ") received MyEvent" << std::endl;
        if (!_id_pong)    // is Pong Actor
            reply(event); // reply the event to the Ping Actor
        kill();           // Ping or Pong will die after receiving MyEvent
    }
};

#endif