#include "system/actor/IActor.h"
#include "system/actor/Types.h"

#ifdef NOLOG
#ifndef NOCOUT
std::mutex cube::io::io_lock;
#endif
#endif