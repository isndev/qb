// <qb/actorid.h> ALONE. ActorId's public ctors are the default one and the uint32 one; the
// two-argument (ServiceId, CoreId) form is protected on purpose. BroadcastId is the public
// way to build one from a core index.
#include <qb/actorid.h>
#include <cstdio>

int
main() {
    const qb::ActorId      none;                     // == ActorId::NotFound
    const qb::ActorId      raw(0x00010002u);
    const qb::BroadcastId  all(1);
    std::printf("%u %u %u %u\n", (unsigned) none.index(), (unsigned) raw.sid(),
                (unsigned) raw.index(), (unsigned) all.index());
    return 0;
}
