// <qb/core/patterns.h> ALONE.
#include <qb/core/patterns.h>

struct CorePatternsEvent : qb::Event {
    int seq;
};

class CorePatternsActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<CorePatternsEvent>(*this);
        co_return true;
    }
    void
    on(const CorePatternsEvent &) {}
};

void
use_push(const CorePatternsActor &a, qb::ActorId dest) {
    a.push<CorePatternsEvent>(dest);
}

int
main() {
    return 0;
}
