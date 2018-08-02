#include "io.h"

#ifdef NOLOG
cube::io::LogLevel cube::io::log::level = cube::io::LogLevel::WARN;
#ifndef NOCOUT
std::mutex cube::io::io_lock;
#endif
#endif