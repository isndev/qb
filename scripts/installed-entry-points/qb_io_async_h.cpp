// <qb/io/async.h> ALONE -- plus a listener touch, so the qb-io archive is actually consulted.
#include <qb/io/async.h>

qb::io::async::task<int>
produce() {
    co_return 7;
}

int
main() {
    qb::io::async::init();
    auto t = produce();
    (void) t;
    return 0;
}
