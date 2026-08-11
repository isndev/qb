// <qb/icallback.h> ALONE. The interface's own contract is `on(qb::LoopEvent const&)`; the
// vtable and qb::LoopEvent both have to be reachable from this one include.
#include <qb/icallback.h>
#include <cstdio>

struct Ticker : qb::ICallback {
    std::uint64_t seen{0};
    void
    on(qb::LoopEvent const &loop) override {
        seen = loop.iteration;
    }
};

int
main() {
    Ticker        t;
    qb::LoopEvent loop{};
    loop.iteration = 3;
    static_cast<qb::ICallback &>(t).on(loop); // through the vtable
    std::printf("%llu\n", (unsigned long long) t.seen);
    return 0;
}
