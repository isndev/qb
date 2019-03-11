
#ifndef QB_UTILS_NOCOPY_H
#define QB_UTILS_NOCOPY_H

namespace qb {
    struct nocopy {
        nocopy() = default;
        nocopy(nocopy const &) = delete;
        nocopy(nocopy const &&) = delete;
        nocopy &operator=(nocopy const &) = delete;
    };
}

#endif //QB_UTILS_NOCOPY_H
