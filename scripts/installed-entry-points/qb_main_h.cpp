// <qb/main.h> ALONE. This is the umbrella qb's own README opens with, and until 3.0 it could
// not link `qb::Actor::push<E>` -- the member template is declared by Pipe.h, which Main.h
// reaches, but defined in Actor.tpp, which only qb/actor.h, qb/patterns.h and
// qb/core/patterns.h pulled. Compiles clean; the linker is the only thing that ever said so.
#include <qb/main.h>

struct MainOnlyEvent : qb::Event {
    int seq;
};

class MainOnlyActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<MainOnlyEvent>(*this);
        co_return true;
    }
    void
    on(const MainOnlyEvent &) {}
};

// ODR-use, through a non-inline function so no optimiser can fold the call away.
void
use_push(const MainOnlyActor &a, qb::ActorId dest) {
    a.push<MainOnlyEvent>(dest);
}

int
main() {
    qb::Main engine;
    (void) engine.core(0);
    return 0;
}
