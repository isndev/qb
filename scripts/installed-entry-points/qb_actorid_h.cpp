// <qb/actorid.h> ALONE. The public raw codec keeps the full slot/core/generation identity;
// the two-argument (ServiceId, CoreId) form is protected on purpose. BroadcastId is the
// public way to build one from a core index.
#include <qb/actorid.h>
#include <cstdint>
#include <cstdio>

int
main() {
    const qb::ActorId     none; // == ActorId::NotFound
    const qb::ActorId     raw = qb::ActorId::from_raw(std::uint64_t{0x0000000101000002ULL});
    const qb::BroadcastId all(1);
    std::printf("%u %u %u %u\n", (unsigned) none.index(), (unsigned) raw.sid(), (unsigned) raw.index(), (unsigned) all.index());
    return raw.raw64() == std::uint64_t{0x0000000101000002ULL} ? 0 : 1;
}
