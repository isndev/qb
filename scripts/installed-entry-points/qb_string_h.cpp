#include <qb/string.h>
#include <cstdio>

int
main() {
    qb::string<32> s{"entry"};
    std::printf("%zu\n", s.size());
    return 0;
}
