// <qb/core/Actor.h> ALONE -- the header named after the class, entered directly.
//
// DELIBERATE SCOPE. This TU does NOT ODR-use qb::Actor::push<E>, and that is not an oversight:
// Actor.h by design does not pull Actor.tpp (see the comment at qb/patterns.h:19). It cannot --
// Actor.tpp needs a COMPLETE qb::VirtualCore, and VirtualCore.h is what drags <windows.h>,
// WIN32_LEAN_AND_MEAN and NOMINMAX into a TU. Making every actor TU pay that is a worse trade
// than the umbrella rule. `push<E>` is the umbrella umbrellas' job, and qb_main_h.cpp /
// qb_actor_h.cpp / qb_patterns_h.cpp / qb_core_patterns_h.cpp each hold it to that.
//
// What this file DOES hold Actor.h to: entering here must still reach the archive. is_alive(),
// getName() and kill() are non-template members declared here and defined in libqb-core.
#include <qb/core/Actor.h>
#include <cstdio>

class CoreActorActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

void
use_archive(const CoreActorActor &a) {
    std::printf("%d %.*s\n", (int) a.is_alive(), (int) a.getName().size(), a.getName().data());
    a.kill();
}

int
main() {
    return 0;
}
