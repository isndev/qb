// <qb/actor.h> ALONE -- the umbrella that DID link `push<E>` while <qb/main.h> did not.
// Keeping both in the matrix is the point: they must agree.
#include <qb/actor.h>

struct ActorOnlyEvent : qb::Event {
    int seq;
};

class ActorOnlyActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<ActorOnlyEvent>(*this);
        co_return true;
    }
    void
    on(const ActorOnlyEvent &) {}
};

void
use_push(const ActorOnlyActor &a, qb::ActorId dest) {
    a.push<ActorOnlyEvent>(dest);
}

int
main() {
    return 0;
}
