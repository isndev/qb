// <qb/testkit.h> ALONE.
#include <qb/testkit.h>

struct KitEvent : qb::Event {};

bool
use_kit() {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<KitEvent>(0);
    const auto        run   = kit.run([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        (void) co_await qb::test::expect_none<KitEvent>(ctx, probe, std::chrono::milliseconds(1));
        (void) co_await qb::test::expect<KitEvent>(ctx, probe, std::chrono::milliseconds(1));
    });
    return static_cast<bool>(run);
}

int
main() {
    return 0;
}
