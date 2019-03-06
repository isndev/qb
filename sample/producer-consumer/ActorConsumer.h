#include <vector>
#include <cube/actor.h>
#include "MyEvent.h"

#ifndef ACTORCONSUMER_H_
# define ACTORCONSUMER_H_

class ActorConsumer
        : public qb::Actor     // /!\ should inherit from cube actor
        , public qb::ICallback // (optional) required to register actor callback
{
    uint64_t timer;
    uint64_t counter;

    void reset_timer() {
        timer = time() + qb::Timestamp::seconds(1).nanoseconds();
    }

public:
    ActorConsumer() = default;             // default constructor
    ~ActorConsumer() = default;

    // will call this function before adding MyActor
    virtual bool onInit() override final {
        registerEvent<MyEvent>(*this);     // will listen MyEvent
        registerCallback(*this);           // each core loop will call onCallback
        reset_timer();
        return true;                       // init ok, MyActor will be added
    }

    // will call this function each core loop
    virtual void onCallback() override final {
        if (time() > timer) {
            //qb::io::cout() << "Consumer(" << id() << ") received " << counter << "/s" << std::endl;
            LOG_INFO << "Consumer(" << id() << ") received " << counter << "/s";
            reset_timer();
            counter = 0;
        }
    }

    // will call this function when MyActor received MyEvent
    void on(MyEvent const &) {
        ++counter;
    }
};

#endif