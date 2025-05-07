/**
 * @file qb/io.h
 * @brief Core I/O and logging utilities for the qb framework
 *
 * This file provides basic I/O functionality and logging utilities
 * for the qb framework. It includes a thread-safe console output class
 * and logging macros that can be configured at compile time.
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
 *      ERROR,
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
    cerr() = default;
    cerr(cerr const &) = delete;
    ~cerr();
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
#ifdef QB_STDOUT_LOG
/**
 * @brief Debug-level log macro
 * @param X Message to log
 */
#define LOG_DEBUG(X) qb::io::cout() << X << std::endl;
/**
 * @brief Verbose-level log macro
 * @param X Message to log
 */
#define LOG_VERB(X) qb::io::cout() << X << std::endl;
/**
 * @brief Info-level log macro
 * @param X Message to log
 */
#define LOG_INFO(X) qb::io::cout() << X << std::endl;
/**
 * @brief Warning-level log macro
 * @param X Message to log
 */
#define LOG_WARN(X) qb::io::cout() << X << std::endl;
/**
 * @brief Critical-level log macro
 * @param X Message to log
 */
#define LOG_CRIT(X) qb::io::cout() << X << std::endl;
#else
/**
 * @brief Debug-level log macro (no-op if QB_STDOUT_LOG is not defined)
 * @param X Message to log
 */
#define LOG_DEBUG(X) \
    do {             \
    } while (false)
/**
 * @brief Verbose-level log macro (no-op if QB_STDOUT_LOG is not defined)
 * @param X Message to log
 */
#define LOG_VERB(X) \
    do {            \
    } while (false)
/**
 * @brief Info-level log macro (no-op if QB_STDOUT_LOG is not defined)
 * @param X Message to log
 */
#define LOG_INFO(X) \
    do {            \
    } while (false)
/**
 * @brief Warning-level log macro (no-op if QB_STDOUT_LOG is not defined)
 * @param X Message to log
 */
#define LOG_WARN(X) \
    do {            \
    } while (false)
/**
 * @brief Critical-level log macro (no-op if QB_STDOUT_LOG is not defined)
 * @param X Message to log
 */
#define LOG_CRIT(X) \
    do {            \
    } while (false)
#endif
#endif

#endif // QB_TYPES_H