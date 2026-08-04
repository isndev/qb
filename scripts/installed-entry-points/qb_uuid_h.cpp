#include <qb/uuid.h>
#include <cstdio>

int
main() {
    const auto id = qb::generate_random_uuid();
    std::printf("%zu\n", uuids::to_string(id).size());
    return 0;
}
