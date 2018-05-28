
#ifndef CUBE_HANDLER_H
#define CUBE_HANDLER_H

# include <cstring>
# include <unordered_map>
# include <vector>
# include <limits>
# include <chrono>
# include <thread>

# include "utils/TComposition.h"
# include "system/lockfree/mpsc.h"
# include "system/io.h"

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
        uint64_t __padding__[CUBE_LOCKFREE_CACHELINE_BYTES / sizeof(uint64_t)];
    };

    template <typename T>
    struct type_solver {
        typedef T type;
    };

    template <typename _Handler, typename ..._Core>
    std::size_t nb_core() {
        return (_Core::template Type<_Handler>::type::nb_core + ...);
    }

}

#endif //CUBE_HANDLER_H
