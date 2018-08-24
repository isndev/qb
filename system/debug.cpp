#include "io.h"

#ifdef NOLOG
cube::io::LogLevel cube::io::log::level = cube::io::LogLevel::WARN;

void cube::io::log::setLevel(io::LogLevel lvl) {
    nanolog::set_log_level(lvl);
}

#ifndef NOCOUT
std::mutex cube::io::io_lock;
#endif
#endif