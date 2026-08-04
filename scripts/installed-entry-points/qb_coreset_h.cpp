// <qb/coreset.h> ALONE. build()/resolve()/getSize() are non-template members defined in
// libqb-core, so this entry point is a real link, not a parse.
#include <qb/coreset.h>
#include <cstdio>

int
main() {
    const qb::CoreSet cs = qb::CoreSet::build(2);
    std::printf("%u %u %u\n", cs.getSize(), cs.getNbCore(), (unsigned) cs.resolve(0));
    return 0;
}
