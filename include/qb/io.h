/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_TYPES_H
#define QB_TYPES_H

#include <iostream>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>

#ifdef QB_LOGGER
#include <nanolog/nanolog.h>
#endif

namespace qb {
#ifdef NDEBUG
constexpr static bool debug = false;
#else
constexpr static bool debug = true;
#endif

namespace io {
#ifdef QB_LOGGER
namespace log {
using stream = nanolog::NanoLogLine;
using Level = nanolog::LogLevel;
/*!
 * set log level
 * @param qb::io::log::Level
 */
void setLevel(Level lvl);
/*!
 * @brief init logger
 * @details
 * @param file_path logfile path name
 * @param roll_MB Max roll file size in MB
 * @code
 * enum class LogLevel : uint8_t {
 *      DEBUG,
 *      VERBOSE,
 *      INFO,
 *      WARN,
 *      ERROR,
 *      CRIT };
 * @endcode
 */
void init(std::string const &file_path, uint32_t roll_MB = 128);
} // namespace log
#endif
/*!
 * @class cout
 * @brief thread safe print in std::cout
 * @details
 * example:
 * @code
 * qb::cout() << ... ;
 * @endcode
 * @note prefer using logger than print
 */
class cout {
    static std::mutex io_lock;
    std::stringstream ss;

public:
    cout() = default;
    cout(cout const &) = delete;
    ~cout();

    template <typename T>
    inline std::stringstream &
    operator<<(T const &data) {
        ss << data;
        return ss;
    }
};
} // namespace io
} // namespace qb

#ifndef QB_LOGGER
#ifndef QB_STDOUT_LOG
#    define LOG_DEBUG(X) qb::cout() << X
#    define LOG_VERB(X) qb::cout() << X
#    define LOG_INFO(X) qb::cout() << X
#    define LOG_WARN(X) qb::cout() << X
#    define LOG_CRIT(X) qb::cout() << X
#else
#    define LOG_DEBUG(X)
#    define LOG_VERB(X)
#    define LOG_INFO(X)
#    define LOG_WARN(X)
#    define LOG_CRIT(X)
#endif
#endif

#endif // QB_TYPES_H