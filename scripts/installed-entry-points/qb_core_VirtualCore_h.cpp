// <qb/core/VirtualCore.h> ALONE. Same deliberate scope as qb_core_Actor_h.cpp: the class
// headers promise a complete class plus the archive's non-template members, not the template
// bodies (see qb/patterns.h:19 for the umbrella rule). getIndex()/time()/getCoreSet() are
// declared here and defined in libqb-core, so entering through this header alone must still
// produce a linked program.
#include <qb/core/VirtualCore.h>
#include <cstdio>

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
