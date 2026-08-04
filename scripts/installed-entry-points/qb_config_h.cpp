// <qb/config.h> ALONE -- the configuration/vocabulary header a consumer includes to test
// QB_VERSION and friends before anything else.
#include <qb/config.h>
#include <cstdio>

int
main() {
    std::printf("%d.%d.%d\n", QB_VERSION_MAJOR, QB_VERSION_MINOR, QB_VERSION_PATCH);
    return 0;
}
