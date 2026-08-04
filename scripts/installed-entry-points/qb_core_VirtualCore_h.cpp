// <qb/core/VirtualCore.h> ALONE.
//
// TWO things, and the second one is new in 3.0.
//
// 1. The archive half, unchanged and the same deliberate scope as qb_core_Actor_h.cpp:
//    getIndex()/time()/getCoreSet() are declared here and defined in libqb-core, so entering
//    through this header alone must still produce a LINKED program.
//
// 2. The template half. Through 2.6.0 this header promised a complete class and the archive's
//    non-template members, and nothing else -- the bodies lived in the four `.tpp`. 3.0 retired
//    them, and VirtualCore.h is where qb::Actor's bodies landed, because it is the header that
//    CLOSES the Actor <-> VirtualCore cycle (VirtualCore.h includes Actor.h at :61, so the tail
//    of Actor.h can never see a complete qb::VirtualCore). That makes this file the direct,
//    umbrella-free assertion of the new contract: `push<E>` and `registerEvent<E>` must be
//    emitted by a TU whose ONLY qb include is this one.
//
//    Without it the invariant is only ever tested through <qb/actor.h>, <qb/main.h>,
//    <qb/patterns.h> and <qb/core/patterns.h> -- four umbrellas that could all keep passing the
//    job to a fifth header, which is precisely the shape of the defect (one job including the
//    one combination that hides it) this whole directory exists to prevent.
#include <qb/core/VirtualCore.h>
#include <cstdio>

struct VirtualCoreOnlyEvent : qb::Event {
    int seq;
};

class VirtualCoreOnlyActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<VirtualCoreOnlyEvent>(*this); // Actor::registerEvent<E> body: this header
        co_return true;
    }
    void
    on(const VirtualCoreOnlyEvent &) {}
};

// ODR-use through a non-inline function so no optimiser can fold the call away.
void
use_push(const VirtualCoreOnlyActor &a, qb::ActorId dest) {
    a.push<VirtualCoreOnlyEvent>(dest); // Actor::push<E> body: this header
}

void
use_archive(const qb::VirtualCore &vc) {
    std::printf("%u %llu %zu\n", (unsigned) vc.getIndex(), (unsigned long long) vc.time(),
                vc.getCoreSet().size());
}

int
main() {
    // A documented public knob that lives in this header and in the archive's data.
    qb::VirtualCore::activation_deadline_ns = 5ull * 1000u * 1000u * 1000u;
    return 0;
}
