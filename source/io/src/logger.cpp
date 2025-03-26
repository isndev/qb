/**
 * @file qb/io/src/logger.cpp
 * @brief Implementation of the logging system
 * 
 * This file contains the implementation of the logging system for the QB framework.
 * It provides functionality for initializing the logger, setting log levels,
 * and thread-safe console output.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <qb/io.h>
#ifdef QB_LOGGER
void
qb::io::log::init(std::string const &file_path, uint32_t const roll_MB) {
    nanolog::initialize(nanolog::GuaranteedLogger(), file_path, roll_MB);
}

void
qb::io::log::setLevel(io::log::Level lvl) {
    nanolog::set_log_level(lvl);
}
#endif
std::mutex qb::io::cout::io_lock;

qb::io::cout::~cout() {
    std::lock_guard<std::mutex> lock(io_lock);
    std::cout << ss.str() << std::flush;
}

#ifdef QB_LOGGER
struct LogInitializer {
    static LogInitializer initializer;
    LogInitializer() noexcept {
        qb::io::log::init("./qb", 512);
#    ifdef NDEBUG
        qb::io::log::setLevel(qb::io::log::Level::INFO);
#    else
        qb::io::log::setLevel(qb::io::log::Level::DEBUG);
#    endif
    }
};

LogInitializer LogInitializer::initializer = {};
#endif