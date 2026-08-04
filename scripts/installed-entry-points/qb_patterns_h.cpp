// <qb/patterns.h> ALONE.
#include <qb/patterns.h>

struct PatternsEvent : qb::Event {
    int seq;
};

class PatternsActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<PatternsEvent>(*this);
        co_return true;
    }
    void
    on(const PatternsEvent &) {}
};

void
use_push(const PatternsActor &a, qb::ActorId dest) {
    a.push<PatternsEvent>(dest);
}

int
main() {
    return 0;
}
