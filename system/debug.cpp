#include "io.h"

#ifdef NOLOG
#ifndef NOCOUT
std::mutex cube::io::io_lock;
#endif
#endif