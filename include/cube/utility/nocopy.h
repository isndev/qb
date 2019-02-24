
#ifndef CUBE_UTILS_NOCOPY_H
#define CUBE_UTILS_NOCOPY_H

namespace cube {
    struct nocopy {
        nocopy() = default;
        nocopy(nocopy const &) = delete;
        nocopy(nocopy const &&) = delete;
        nocopy &operator=(nocopy const &) = delete;
    };
}

#endif //CUBE_UTILS_NOCOPY_H
