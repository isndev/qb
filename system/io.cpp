#include "io.h"

void cube::io::log::init(std::string const &dir, std::string const &file, uint32_t const roll_MB) {
    nanolog::initialize(nanolog::GuaranteedLogger(), dir, file, roll_MB);
}

void cube::io::log::setLevel(io::log::Level lvl) {
    nanolog::set_log_level(lvl);
}

std::mutex cube::io::cout::io_lock;

cube::io::cout::~cout() {
    std::lock_guard<std::mutex> lock(io_lock);
    std::cout << ss.str() << std::endl << std::flush;
}

