
#ifndef CUBE_HANDLER_H
#define CUBE_HANDLER_H

#ifdef UNIX
#define __USE_GNU
    #include <sched.h>
    #include <errno.h>
    #include <unistd.h>
    #include <pthread.h>
#endif

#ifdef WIN32
    #include <windows.h>
    #include <process.h>
#endif

# include <cstring>
# include <unordered_map>
# include <vector>
# include <limits>
# include <chrono>
# include <thread>

# include "utils/TComposition.h"
# include "system/lockfree/spsc.h"
# include "system/actor/ActorProxy.h"
# include "system/io.h"

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
        uint64_t __padding__[CUBE_LOCKFREE_CACHELINE_BYTES / sizeof(uint64_t)];
    };

    template <typename T>
    struct type_resolver {
        typedef T type;
    };

}

#endif //CUBE_HANDLER_H
