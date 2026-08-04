// <qb/io/async/coroutine.h> ALONE.
#include <qb/io/async/coroutine.h>

qb::io::async::task<int>
produce() {
    co_return 7;
}

int
main() {
    auto t = produce();
    (void) t;
    return 0;
}
