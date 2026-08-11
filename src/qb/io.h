/**
 * @file qb/io.h
 * @brief Core I/O and logging utilities for the qb framework
 *
 * This file provides basic I/O functionality and logging utilities
 * for the qb framework. It includes a thread-safe console output class
 * and logging macros that can be configured at compile time.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#ifndef QB_TYPES_H
#define QB_TYPES_H

#include <iostream>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>
// This umbrella is a documented entry point and reaches nanolog's operator<< templates, whose
// bodies fork under -fno-exceptions while libqb-io also defines them -- one symbol, two bodies,
// winner decided by link order. So it states the link-time ABI contract itself rather than
// relying on the consumer having included some other qb header first.
#include <qb/utility/abi.h>

#ifdef QB_WITH_LOGGING
#include <qb/vendor/nanolog/nanolog.h>
#endif

namespace qb {
#ifdef NDEBUG
constexpr static bool debug = false;
#else
constexpr static bool debug = true;
#endif

namespace io {
#ifdef QB_WITH_LOGGING
namespace log {
using stream = nanolog::NanoLogLine;
using Level  = nanolog::LogLevel;
/**
 * @brief Set the logging level
 *
 * Configures the minimum severity level for log messages that will be recorded.
 * Messages with a severity level lower than the specified level will be ignored.
 *
 * @param lvl The minimum log level to record
 */
void setLevel(Level lvl);

/**
 * @brief Initialize the logging system
 *
 * Sets up the logging system with the specified file path and roll size.
 * This must be called before any logging operations can be performed.
 *
 * @param file_path Path to the log file
 * @param roll_MB Maximum size of a log file in MB before rolling to a new file
 *
 * Available log levels:
 * @code
 * enum class LogLevel : uint8_t {
 *      DEBUG,
 *      VERBOSE,
 *      INFO,
 *      WARN,
 *      CRIT };
 * @endcode
 */
void init(std::string const &file_path, uint32_t roll_MB = 128);
} // namespace log
#endif

/**
 * @class cout
 * @brief Thread-safe console output class
 *
 * This class provides a thread-safe wrapper around std::cout. It uses
 * a mutex to ensure that output operations from multiple threads don't
 * interleave, resulting in garbled output.
 *
 * Example usage:
 * @code
 * qb::io::cout() << "Thread " << thread_id << " is running";
 * @endcode
 *
 * @note For production code, it's preferable to use the logging system
 *       rather than direct console output.
 */
class cout {
    static std::mutex io_lock;
    std::stringstream ss;

public:
    /**
     * @brief Default constructor
     */
    cout() = default;

    /**
     * @brief Deleted copy constructor
     */
    cout(cout const &) = delete;

    /**
     * @brief Destructor that flushes output
     *
     * When the cout object is destroyed, its buffered content is
     * output to std::cout in a thread-safe manner.
     */
    ~cout();

    /**
     * @brief Stream insertion operator
     *
     * Allows data to be inserted into the output stream using the
     * standard C++ stream insertion syntax.
     *
     * @tparam T Type of data to insert
     * @param data The data to insert
     * @return Reference to the internal stringstream
     */
    template <typename T>
    inline std::stringstream &
    operator<<(T const &data) {
        ss << data;
        return ss;
    }

    inline std::stringstream &
    operator<<(std::ostream &(*manip)(std::ostream &) ) {
        ss << manip;
        return ss;
    }
};

/**
 * @class cerr
 * @brief Thread-safe error output class
 *
 * This class provides a thread-safe wrapper around std::cerr. It uses
 * a mutex to ensure that output operations from multiple threads don't
 * interleave, resulting in garbled output.
 *
 * Example usage:
 * @code
 * qb::io::cerr() << "Error: " << error_message;
 * @endcode
 */
class cerr {
    static std::mutex io_lock;
    std::stringstream ss;

public:
    cerr()             = default;
    cerr(cerr const &) = delete;
    ~cerr();
    template <typename T>
    inline std::stringstream &
    operator<<(T const &data) {
        ss << data;
        return ss;
    }

    inline std::stringstream &
    operator<<(std::ostream &(*manip)(std::ostream &) ) {
        ss << manip;
        return ss;
    }
};
} // namespace io
} // namespace qb

#ifndef QB_WITH_LOGGING
#ifdef QB_STDOUT_LOGGING
/**
 * @brief Debug-level log macro (qb's own spelling; `QB_LOG_DEBUG` aliases it)
 * @param X Message to log
 */
#define QB_LOG_DEBUG(X)                   \
    do {                                  \
        qb::io::cout() << X << std::endl; \
    } while (false)
/**
 * @brief Verbose-level log macro
 * @param X Message to log
 */
#define QB_LOG_VERB(X)                    \
    do {                                  \
        qb::io::cout() << X << std::endl; \
    } while (false)
/**
 * @brief Info-level log macro
 * @param X Message to log
 */
#define QB_LOG_INFO(X)                    \
    do {                                  \
        qb::io::cout() << X << std::endl; \
    } while (false)
/**
 * @brief Warning-level log macro
 * @param X Message to log
 */
#define QB_LOG_WARN(X)                    \
    do {                                  \
        qb::io::cout() << X << std::endl; \
    } while (false)
/**
 * @brief Critical-level log macro
 * @param X Message to log
 */
#define QB_LOG_CRIT(X)                    \
    do {                                  \
        qb::io::cout() << X << std::endl; \
    } while (false)
#else
/**
 * @brief Debug-level log macro (qb's own spelling; `QB_LOG_DEBUG` aliases it) (no-op if QB_STDOUT_LOGGING is not defined)
 * @param X Message to log
 */
#define QB_LOG_DEBUG(X) \
    do {                \
    } while (false)
/**
 * @brief Verbose-level log macro (no-op if QB_STDOUT_LOGGING is not defined)
 * @param X Message to log
 */
#define QB_LOG_VERB(X) \
    do {               \
    } while (false)
/**
 * @brief Info-level log macro (no-op if QB_STDOUT_LOGGING is not defined)
 * @param X Message to log
 */
#define QB_LOG_INFO(X) \
    do {               \
    } while (false)
/**
 * @brief Warning-level log macro (no-op if QB_STDOUT_LOGGING is not defined)
 * @param X Message to log
 */
#define QB_LOG_WARN(X) \
    do {               \
    } while (false)
/**
 * @brief Critical-level log macro (no-op if QB_STDOUT_LOGGING is not defined)
 * @param X Message to log
 */
#define QB_LOG_CRIT(X) \
    do {               \
    } while (false)
#endif
#endif

/*
 * Legacy unprefixed spellings.
 *
 * qb's public macros are QB_LOG_DEBUG / QB_LOG_VERB / QB_LOG_INFO / QB_LOG_WARN / QB_LOG_CRIT.
 * The unprefixed names are kept, because they are what every qb program written before 3.0.0
 * calls -- but they are now *aliases*, and each one is guarded by its own `#ifndef`.
 *
 * That guard is the fix. `QB_LOG_INFO`, `QB_LOG_DEBUG` and `QB_LOG_CRIT` are also POSIX <syslog.h> names,
 * and this header is reached by every consumer of <qb/io.h>, <qb/main.h>, <qb/actor.h> and every
 * qbm umbrella. Before 3.0.0 qb defined them unconditionally, so a consumer with its own
 * `QB_LOG_INFO` had it silently REPLACED -- measured with the exact `-isystem` line qb's CMake
 * package exports, at -Wall -Wextra: **zero warnings**, and the consumer's log line simply
 * stopped appearing. (With `-I` instead of `-isystem` the same build reports
 * `warning: 'QB_LOG_INFO' macro redefined`, which is why the real integration never saw it.)
 * With the guard, a consumer who defined the name first keeps their own definition, and a
 * consumer who defines it afterwards gets the ordinary -Wmacro-redefined diagnostic.
 *
 * Define QB_NO_LEGACY_LOG_MACROS to suppress the aliases entirely and take the five names back.
 * qb's own headers never use them.
 */
#ifndef QB_NO_LEGACY_LOG_MACROS
#ifndef LOG_DEBUG
#define LOG_DEBUG(X) QB_LOG_DEBUG(X)
#endif
#ifndef LOG_VERB
#define LOG_VERB(X) QB_LOG_VERB(X)
#endif
#ifndef LOG_INFO
#define LOG_INFO(X) QB_LOG_INFO(X)
#endif
#ifndef LOG_WARN
#define LOG_WARN(X) QB_LOG_WARN(X)
#endif
#ifndef LOG_CRIT
#define LOG_CRIT(X) QB_LOG_CRIT(X)
#endif
#endif /* QB_NO_LEGACY_LOG_MACROS */

#endif // QB_TYPES_H