/**
 * @file qb/system/timestamp.h
 * @brief High-precision timing utilities
 * 
 * This file provides a set of classes for precise time handling, measurement, 
 * and representation. It includes platform-independent implementations for
 * time spans and timestamps with nanosecond precision.
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
 * @ingroup System
 */

#ifndef FEATURES_TIMESTAMP_H
#define FEATURES_TIMESTAMP_H

#include <chrono>
#include <ctime>
#include <exception>
#if defined(__APPLE__)
#    include <mach/mach.h>
#    include <mach/mach_time.h>
#    include <math.h>
#    include <sys/time.h>
#    include <time.h>
#elif defined(unix) || defined(__unix) || defined(__unix__)
#    include <ctime>
#elif defined(_WIN32) || defined(_WIN64)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif // !WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif
#include <iostream>
#include <thread>
#include <qb/io.h>

namespace qb {

/**
 * @class Timespan
 * @brief Represents a duration with nanosecond precision
 * 
 * Timespan provides a platform-independent way to represent time durations with
 * high precision. It supports arithmetic operations and various time unit
 * conversions (days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
 */
class Timespan {
public:
    /**
     * @brief Default constructor, initializes duration to zero
     */
    Timespan() noexcept
        : _duration(0) {}

    /**
     * @brief Constructs a timespan with the specified duration in nanoseconds
     * 
     * @param duration Duration in nanoseconds
     */
    explicit Timespan(int64_t duration) noexcept
        : _duration(duration) {}
    Timespan(const Timespan &) noexcept = default;
    Timespan(Timespan &&) noexcept = default;

    ~Timespan() noexcept = default;

    /**
     * @brief Assigns a duration in nanoseconds to this timespan
     * 
     * @param duration Duration in nanoseconds
     * @return Reference to this timespan after assignment
     */
    Timespan &
    operator=(int64_t duration) noexcept {
        _duration = duration;
        return *this;
    }
    Timespan &operator=(const Timespan &) noexcept = default;
    Timespan &operator=(Timespan &&) noexcept = default;

    // Timespan offset operations
    /**
     * @brief Unary plus operator
     * 
     * @return A copy of this timespan with the same duration
     */
    Timespan
    operator+() const {
        return Timespan(+_duration);
    }
    
    /**
     * @brief Unary minus operator
     * 
     * @return A timespan with negated duration
     */
    Timespan
    operator-() const {
        return Timespan(-_duration);
    }
    
    /**
     * @brief Adds a duration in nanoseconds to this timespan
     * 
     * @param offset Duration in nanoseconds to add
     * @return Reference to this timespan after addition
     */
    Timespan &
    operator+=(int64_t offset) noexcept {
        _duration += offset;
        return *this;
    }
    
    /**
     * @brief Adds another timespan to this timespan
     * 
     * @param offset Timespan to add
     * @return Reference to this timespan after addition
     */
    Timespan &
    operator+=(const Timespan &offset) noexcept {
        _duration += offset.total();
        return *this;
    }
    
    /**
     * @brief Subtracts a duration in nanoseconds from this timespan
     * 
     * @param offset Duration in nanoseconds to subtract
     * @return Reference to this timespan after subtraction
     */
    Timespan &
    operator-=(int64_t offset) noexcept {
        _duration -= offset;
        return *this;
    }
    
    /**
     * @brief Subtracts another timespan from this timespan
     * 
     * @param offset Timespan to subtract
     * @return Reference to this timespan after subtraction
     */
    Timespan &
    operator-=(const Timespan &offset) noexcept {
        _duration -= offset.total();
        return *this;
    }

    // Friend operators for Timespan arithmetic
    friend Timespan
    operator+(const Timespan &timespan, int64_t offset) noexcept {
        return Timespan(timespan.total() + offset);
    }
    friend Timespan
    operator+(int64_t offset, const Timespan &timespan) noexcept {
        return Timespan(offset + timespan.total());
    }
    friend Timespan
    operator+(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return Timespan(timespan1.total() + timespan2.total());
    }
    friend Timespan
    operator-(const Timespan &timespan, int64_t offset) noexcept {
        return Timespan(timespan.total() - offset);
    }
    friend Timespan
    operator-(int64_t offset, const Timespan &timespan) noexcept {
        return Timespan(offset - timespan.total());
    }
    friend Timespan
    operator-(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return Timespan(timespan1.total() - timespan2.total());
    }

    // Timespan comparison
    friend bool
    operator==(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() == offset;
    }
    friend bool
    operator==(int64_t offset, const Timespan &timespan) noexcept {
        return offset == timespan.total();
    }
    friend bool
    operator==(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() == timespan2.total();
    }
    friend bool
    operator!=(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() != offset;
    }
    friend bool
    operator!=(int64_t offset, const Timespan &timespan) noexcept {
        return offset != timespan.total();
    }
    friend bool
    operator!=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() != timespan2.total();
    }
    friend bool
    operator>(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() > offset;
    }
    friend bool
    operator>(int64_t offset, const Timespan &timespan) noexcept {
        return offset > timespan.total();
    }
    friend bool
    operator>(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() > timespan2.total();
    }
    friend bool
    operator<(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() < offset;
    }
    friend bool
    operator<(int64_t offset, const Timespan &timespan) noexcept {
        return offset < timespan.total();
    }
    friend bool
    operator<(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() < timespan2.total();
    }
    friend bool
    operator>=(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() >= offset;
    }
    friend bool
    operator>=(int64_t offset, const Timespan &timespan) noexcept {
        return offset >= timespan.total();
    }
    friend bool
    operator>=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() >= timespan2.total();
    }
    friend bool
    operator<=(const Timespan &timespan, int64_t offset) noexcept {
        return timespan.total() <= offset;
    }
    friend bool
    operator<=(int64_t offset, const Timespan &timespan) noexcept {
        return offset <= timespan.total();
    }
    friend bool
    operator<=(const Timespan &timespan1, const Timespan &timespan2) noexcept {
        return timespan1.total() <= timespan2.total();
    }

    /**
     * @brief Converts to std::chrono duration
     * 
     * @return A std::chrono::nanoseconds duration
     */
    [[nodiscard]] std::chrono::duration<int64_t, std::nano>
    chrono() const noexcept {
        return std::chrono::nanoseconds(_duration);
    }

    /**
     * @brief Creates a Timespan from a std::chrono duration
     * 
     * @tparam Rep Type of the count representation
     * @tparam Period Type of the period representation
     * @param duration The std::chrono duration to convert
     * @return A Timespan representing the duration
     */
    template <class Rep, class Period>
    static Timespan
    chrono(const std::chrono::duration<Rep, Period> &duration) noexcept {
        return Timespan(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

    /**
     * @brief Gets the duration in days
     * 
     * @return Number of days
     */
    [[nodiscard]] int64_t
    days() const noexcept {
        return _duration / (24 * 60 * 60 * 1000000000ll);
    }
    
    /**
     * @brief Gets the duration in hours
     * 
     * @return Number of hours
     */
    [[nodiscard]] int64_t
    hours() const noexcept {
        return _duration / (60 * 60 * 1000000000ll);
    }
    
    /**
     * @brief Gets the duration in minutes
     * 
     * @return Number of minutes
     */
    [[nodiscard]] int64_t
    minutes() const noexcept {
        return _duration / (60 * 1000000000ll);
    }
    
    /**
     * @brief Gets the duration in seconds
     * 
     * @return Number of seconds
     */
    [[nodiscard]] int64_t
    seconds() const noexcept {
        return _duration / 1000000000;
    }
    
    /**
     * @brief Gets the duration in milliseconds
     * 
     * @return Number of milliseconds
     */
    [[nodiscard]] int64_t
    milliseconds() const noexcept {
        return _duration / 1000000;
    }
    
    /**
     * @brief Gets the duration in microseconds
     * 
     * @return Number of microseconds
     */
    [[nodiscard]] int64_t
    microseconds() const noexcept {
        return _duration / 1000;
    }
    
    /**
     * @brief Gets the duration in nanoseconds
     * 
     * @return Number of nanoseconds
     */
    [[nodiscard]] int64_t
    nanoseconds() const noexcept {
        return _duration;
    }
    
    /**
     * @brief Gets the total duration in nanoseconds
     * 
     * @return Duration in nanoseconds
     */
    [[nodiscard]] int64_t
    total() const noexcept {
        return _duration;
    }

    /**
     * @brief Creates a Timespan representing a specified number of days
     * 
     * @param days Number of days
     * @return A Timespan representing the specified duration
     */
    static Timespan
    days(int64_t days) noexcept {
        return Timespan(days * 24 * 60 * 60 * 1000000000ll);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of hours
     * 
     * @param hours Number of hours
     * @return A Timespan representing the specified duration
     */
    static Timespan
    hours(int64_t hours) noexcept {
        return Timespan(hours * 60 * 60 * 1000000000ll);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of minutes
     * 
     * @param minutes Number of minutes
     * @return A Timespan representing the specified duration
     */
    static Timespan
    minutes(int64_t minutes) noexcept {
        return Timespan(minutes * 60 * 1000000000ll);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of seconds
     * 
     * @param seconds Number of seconds
     * @return A Timespan representing the specified duration
     */
    static Timespan
    seconds(int64_t seconds) noexcept {
        return Timespan(seconds * 1000000000);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of milliseconds
     * 
     * @param milliseconds Number of milliseconds
     * @return A Timespan representing the specified duration
     */
    static Timespan
    milliseconds(int64_t milliseconds) noexcept {
        return Timespan(milliseconds * 1000000);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of microseconds
     * 
     * @param microseconds Number of microseconds
     * @return A Timespan representing the specified duration
     */
    static Timespan
    microseconds(int64_t microseconds) noexcept {
        return Timespan(microseconds * 1000);
    }
    
    /**
     * @brief Creates a Timespan representing a specified number of nanoseconds
     * 
     * @param nanoseconds Number of nanoseconds
     * @return A Timespan representing the specified duration
     */
    static Timespan
    nanoseconds(int64_t nanoseconds) noexcept {
        return Timespan(nanoseconds);
    }
    
    /**
     * @brief Creates a zero Timespan
     * 
     * @return A Timespan with zero duration
     */
    static Timespan
    zero() noexcept {
        return Timespan(0);
    }

private:
    int64_t _duration; ///< Duration in nanoseconds
};

/**
 * @class Timestamp
 * @brief Represents a point in time with nanosecond precision
 * 
 * Timestamp provides a platform-independent way to represent moments in time
 * with high precision. It supports arithmetic operations with Timespan objects
 * and provides various time unit conversions.
 */
class Timestamp {
public:
    /**
     * @brief Default constructor, initializes to epoch
     */
    Timestamp() noexcept
        : _timestamp(epoch()) {}

    /**
     * @brief Constructs a timestamp with the specified value in nanoseconds
     * 
     * @param timestamp Value in nanoseconds since epoch
     */
    explicit Timestamp(uint64_t timestamp) noexcept
        : _timestamp(timestamp) {}
    Timestamp(const Timestamp &) noexcept = default;
    Timestamp(Timestamp &&) noexcept = default;
    ~Timestamp() noexcept = default;

    /**
     * @brief Assigns a timestamp value in nanoseconds to this timestamp
     * 
     * @param timestamp Value in nanoseconds since epoch
     * @return Reference to this timestamp after assignment
     */
    Timestamp &
    operator=(uint64_t timestamp) noexcept {
        _timestamp = timestamp;
        return *this;
    }
    Timestamp &operator=(const Timestamp &) noexcept = default;
    Timestamp &operator=(Timestamp &&) noexcept = default;

    // Timestamp offset operations
    Timestamp &
    operator+=(int64_t offset) noexcept {
        _timestamp += offset;
        return *this;
    }
    Timestamp &
    operator+=(const Timespan &offset) noexcept {
        _timestamp += offset.total();
        return *this;
    }

    Timestamp &
    operator-=(int64_t offset) noexcept {
        _timestamp -= offset;
        return *this;
    }
    Timestamp &
    operator-=(const Timespan &offset) noexcept {
        _timestamp -= offset.total();
        return *this;
    }

    friend Timestamp
    operator+(const Timestamp &timestamp, int64_t offset) noexcept {
        return Timestamp(timestamp.total() + offset);
    }
    friend Timestamp
    operator+(int64_t offset, const Timestamp &timestamp) noexcept {
        return Timestamp(offset + timestamp.total());
    }
    friend Timestamp
    operator+(const Timestamp &timestamp, const Timespan &offset) noexcept {
        return Timestamp(timestamp.total() + offset.total());
    }
    friend Timestamp
    operator+(const Timespan &offset, const Timestamp &timestamp) noexcept {
        return Timestamp(offset.total() + timestamp.total());
    }

    friend Timestamp
    operator-(const Timestamp &timestamp, int64_t offset) noexcept {
        return Timestamp(timestamp.total() - offset);
    }
    friend Timestamp
    operator-(int64_t offset, const Timestamp &timestamp) noexcept {
        return Timestamp(offset - timestamp.total());
    }
    friend Timestamp
    operator-(const Timestamp &timestamp, const Timespan &offset) noexcept {
        return Timestamp(timestamp.total() - offset.total());
    }
    friend Timestamp
    operator-(const Timespan &offset, const Timestamp &timestamp) noexcept {
        return Timestamp(offset.total() - timestamp.total());
    }

    friend Timespan
    operator-(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return Timespan(timestamp1.total() - timestamp2.total());
    }

    // Friend operators for Timestamp comparison
    friend bool
    operator==(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() == timestamp2;
    }
    friend bool
    operator==(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 == timestamp2.total();
    }
    friend bool
    operator==(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() == timestamp2.total();
    }

    friend bool
    operator!=(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() != timestamp2;
    }
    friend bool
    operator!=(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 != timestamp2.total();
    }
    friend bool
    operator!=(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() != timestamp2.total();
    }

    friend bool
    operator>(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() > timestamp2;
    }
    friend bool
    operator>(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 > timestamp2.total();
    }
    friend bool
    operator>(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() > timestamp2.total();
    }

    friend bool
    operator<(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() < timestamp2;
    }
    friend bool
    operator<(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 < timestamp2.total();
    }
    friend bool
    operator<(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() < timestamp2.total();
    }

    friend bool
    operator>=(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() >= timestamp2;
    }
    friend bool
    operator>=(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 >= timestamp2.total();
    }
    friend bool
    operator>=(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() >= timestamp2.total();
    }

    friend bool
    operator<=(const Timestamp &timestamp1, uint64_t timestamp2) noexcept {
        return timestamp1.total() <= timestamp2;
    }
    friend bool
    operator<=(uint64_t timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1 <= timestamp2.total();
    }
    friend bool
    operator<=(const Timestamp &timestamp1, const Timestamp &timestamp2) noexcept {
        return timestamp1.total() <= timestamp2.total();
    }

    [[nodiscard]] std::chrono::time_point<std::chrono::system_clock,
                                          std::chrono::duration<uint64_t, std::nano>>
    chrono() const noexcept {
        return std::chrono::time_point<std::chrono::system_clock>() +
               std::chrono::nanoseconds(_timestamp);
    }
    template <class Clock, class Duration>
    static Timestamp
    chrono(const std::chrono::time_point<Clock, Duration> &time_point) noexcept {
        return Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             time_point.time_since_epoch())
                             .count());
    }

    /**
     * @brief Gets the time in days since epoch
     * 
     * @return Number of days
     */
    [[nodiscard]] uint64_t
    days() const noexcept {
        return _timestamp / (24 * 60 * 60 * 1000000000ull);
    }
    [[nodiscard]] uint64_t
    hours() const noexcept {
        return _timestamp / (60 * 60 * 1000000000ull);
    }
    [[nodiscard]] uint64_t
    minutes() const noexcept {
        return _timestamp / (60 * 1000000000ull);
    }
    [[nodiscard]] uint64_t
    seconds() const noexcept {
        return _timestamp / 1000000000;
    }
    [[nodiscard]] uint64_t
    milliseconds() const noexcept {
        return _timestamp / 1000000;
    }
    [[nodiscard]] uint64_t
    microseconds() const noexcept {
        return _timestamp / 1000;
    }
    [[nodiscard]] uint64_t
    nanoseconds() const noexcept {
        return _timestamp;
    }

    /**
     * @brief Gets the total time in nanoseconds since epoch
     * 
     * @return Time in nanoseconds
     */
    [[nodiscard]] uint64_t
    total() const noexcept {
        return _timestamp;
    }

    /**
     * @brief Creates a Timestamp at a specified number of days since epoch
     * 
     * @param days Number of days
     * @return A Timestamp at the specified time
     */
    static Timestamp
    days(int64_t days) noexcept {
        return Timestamp(days * 24 * 60 * 60 * 1000000000ull);
    }
    static Timestamp
    hours(int64_t hours) noexcept {
        return Timestamp(hours * 60 * 60 * 1000000000ull);
    }
    static Timestamp
    minutes(int64_t minutes) noexcept {
        return Timestamp(minutes * 60 * 1000000000ull);
    }
    static Timestamp
    seconds(int64_t seconds) noexcept {
        return Timestamp(seconds * 1000000000);
    }
    static Timestamp
    milliseconds(int64_t milliseconds) noexcept {
        return Timestamp(milliseconds * 1000000);
    }
    static Timestamp
    microseconds(int64_t microseconds) noexcept {
        return Timestamp(microseconds * 1000);
    }
    static Timestamp
    nanoseconds(int64_t nanoseconds) noexcept {
        return Timestamp(nanoseconds);
    }

    /**
     * @brief Gets the epoch (time zero) value
     * 
     * @return Zero as nanoseconds
     */
    static uint64_t
    epoch() noexcept {
        return 0;
    }
    static uint64_t
    nano() {
        // Store system time and steady time on first call
        static const std::chrono::time_point<std::chrono::system_clock>
            clk_system_start = std::chrono::system_clock::now();
        static const std::chrono::time_point<std::chrono::steady_clock>
            clk_steady_start = std::chrono::steady_clock::now();

        // Nano timestamp is (system_start + (steady_now - steady_start))
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   clk_system_start.time_since_epoch() +
                   (std::chrono::steady_clock::now().time_since_epoch() -
                    clk_steady_start.time_since_epoch()))
            .count();
    }
    static uint64_t
    rdts() {
#if defined(_MSC_VER)
        return __rdtsc();
#elif defined(__i386__)
        uint64_t x;
        __asm__ volatile(".byte 0x0f, 0x31" : "=A"(x));
        return x;
#elif defined(__x86_64__)
        unsigned hi, lo;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)lo) | (((uint64_t)hi) << 32u);
#else
        return 0;
#endif
    }

protected:
    uint64_t _timestamp; ///< Time in nanoseconds since epoch
};

/**
 * @class UtcTimestamp
 * @brief Represents a UTC timestamp with nanosecond precision
 * 
 * Extends Timestamp to specifically represent times in Coordinated Universal Time (UTC).
 */
class UtcTimestamp : public Timestamp {
public:
    using Timestamp::Timestamp;

    /**
     * @brief Default constructor, initializes to current UTC time
     */
    UtcTimestamp()
        : Timestamp(Timestamp::nano()) {}
        
    /**
     * @brief Constructs a UTC timestamp from another timestamp
     * 
     * @param timestamp Source timestamp
     */
    UtcTimestamp(const Timestamp &timestamp)
        : Timestamp(timestamp) {}
};

/**
 * @class LocalTimestamp
 * @brief Represents a local timestamp with nanosecond precision
 * 
 * Extends Timestamp to specifically represent times in the local timezone.
 */
class LocalTimestamp : public Timestamp {
public:
    using Timestamp::Timestamp;

    /**
     * @brief Default constructor, initializes to current local time
     */
    LocalTimestamp()
        : Timestamp(Timestamp::nano()) {}
        
    /**
     * @brief Constructs a local timestamp from another timestamp
     * 
     * @param timestamp Source timestamp
     */
    LocalTimestamp(const Timestamp &timestamp)
        : Timestamp(timestamp) {}
};

/**
 * @class NanoTimestamp
 * @brief Represents a high-precision nanosecond timestamp
 * 
 * Extends Timestamp to specifically represent high-precision times from
 * the system's high-resolution timer.
 */
class NanoTimestamp : public Timestamp {
public:
    using Timestamp::Timestamp;

    /**
     * @brief Default constructor, initializes to current nanosecond time
     */
    NanoTimestamp()
        : Timestamp(Timestamp::nano()) {}
        
    /**
     * @brief Constructs a nanosecond timestamp from another timestamp
     * 
     * @param timestamp Source timestamp
     */
    NanoTimestamp(const Timestamp &timestamp)
        : Timestamp(timestamp) {}
};

/**
 * @class RdtsTimestamp
 * @brief Represents a timestamp based on CPU's timestamp counter
 * 
 * Extends Timestamp to specifically represent times from the CPU's
 * timestamp counter, which provides very high precision but may
 * vary between CPU cores.
 */
class RdtsTimestamp : public Timestamp {
public:
    using Timestamp::Timestamp;

    /**
     * @brief Default constructor, initializes to current CPU timestamp
     */
    RdtsTimestamp()
        : Timestamp(Timestamp::rdts()) {}
        
    /**
     * @brief Constructs an RDTS timestamp from another timestamp
     * 
     * @param timestamp Source timestamp
     */
    RdtsTimestamp(const Timestamp &timestamp)
        : Timestamp(timestamp) {}
};

/**
 * @class LogTimer
 * @brief Utility for logging execution time of code blocks
 * 
 * Creates a timer that logs the elapsed time when it goes out of scope.
 * Useful for performance measurements and debugging.
 */
class LogTimer {
    const std::string reason; ///< Description of what is being timed
    NanoTimestamp ts;         ///< Start timestamp

public:
    /**
     * @brief Constructs a timer with a descriptive reason
     * 
     * @param reason Description of what is being timed
     */
    inline LogTimer(std::string const &reason)
        : reason(reason)
        , ts() {}
        
    /**
     * @brief Destructor that logs elapsed time
     * 
     * When the timer goes out of scope, logs the elapsed time since construction.
     */
    inline ~LogTimer() {
        qb::io::cout() << reason << ": " << (qb::NanoTimestamp() - ts).microseconds() << "us"
                  << std::endl;
    }
};

} // namespace qb

#endif