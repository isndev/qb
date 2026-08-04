#include <qb/json.h>
#include <cstdio>

int
main() {
    qb::json j;
    j["k"] = 42;
    std::printf("%d\n", (int) j["k"]);
    return 0;
}
