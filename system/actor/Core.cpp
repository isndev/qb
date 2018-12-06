//
// Created by isndev on 12/4/18.
//

#include "Core.h"

cube::io::stream &operator<<(cube::io::stream &os, cube::Core const &core) {
    std::stringstream ss;
    ss << "Core(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    os << ss.str();
    return os;
};