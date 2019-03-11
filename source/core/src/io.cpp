#include <qb/io.h>

void qb::io::log::init(std::string const &file_path, uint32_t const roll_MB) {
    nanolog::initialize(nanolog::GuaranteedLogger(), file_path, roll_MB);
}

void qb::io::log::setLevel(io::log::Level lvl) {
    nanolog::set_log_level(lvl);
}

std::mutex qb::io::cout::io_lock;

qb::io::cout::~cout() {
    std::lock_guard<std::mutex> lock(io_lock);
    std::cout << ss.str() << std::flush;
}

struct LogInitializer {
    static LogInitializer initializer;
    LogInitializer() {
        qb::io::log::init("./qb", 512);
#ifdef NDEBUG
        qb::io::log::setLevel(qb::io::log::Level::INFO);
#else
        qb::io::log::setLevel(qb::io::log::Level::DEBUG);
#endif
    }
};

LogInitializer LogInitializer::initializer = {};

