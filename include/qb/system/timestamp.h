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
#include <cstdint>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <format>
#include <iomanip>
#include <array>
#include <concepts>
#include <optional>
#include <functional>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/time.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <qb/io.h>

// Forward declarations
namespace qb {
class Duration;
class TimePoint;

// Specialized time points
class UtcTimePoint;
class LocalTimePoint;
class HighResTimePoint;
class TscTimePoint;  // TSC = Time Stamp Counter (formerly RDTS)
}

namespace qb {

/**
 * @class Duration
 * @brief Represents a duration with nanosecond precision
 *
 * Duration provides a platform-independent way to represent time durations with
 * high precision. It supports arithmetic operations and various time unit
 * conversions, fully interoperable with std::chrono::duration.
 */
class Duration {
public:
    /// Type used for nanosecond representation
    using rep = int64_t;
    
    /// Underlying chrono duration type (nanoseconds)
    using chrono_duration = std::chrono::duration<rep, std::nano>;
    
    /// Represents zero duration
    static constexpr Duration zero() noexcept { return Duration(0); }
    
    /// Default constructor, initializes to zero
    constexpr Duration() noexcept = default;
    
    /**
     * @brief Constructs a duration with specified nanoseconds
     * @param nanoseconds Duration in nanoseconds
     */
    constexpr explicit Duration(rep nanoseconds) noexcept
        : _duration(nanoseconds) {}
    
    /**
     * @brief Constructs a duration from std::chrono::duration
     * @tparam Rep The count representation type
     * @tparam Period The period type
     * @param duration A std::chrono duration
     */
    template <typename Rep, typename Period>
    constexpr explicit Duration(const std::chrono::duration<Rep, Period>& duration) noexcept
        : _duration(std::chrono::duration_cast<chrono_duration>(duration).count()) {}
        
    // Standard special member functions
    constexpr Duration(const Duration&) noexcept = default;
    constexpr Duration(Duration&&) noexcept = default;
    constexpr Duration& operator=(const Duration&) noexcept = default;
    constexpr Duration& operator=(Duration&&) noexcept = default;
    ~Duration() noexcept = default;
    
    /**
     * @brief Converts to std::chrono::duration
     * @return Equivalent std::chrono::duration in nanoseconds
     */
    [[nodiscard]] constexpr chrono_duration to_chrono() const noexcept {
        return chrono_duration(_duration);
    }
    
    /**
     * @brief Converts to any std::chrono::duration
     * @tparam Duration The target duration type
     * @return Duration converted to the specified std::chrono::duration
     */
    template <typename TargetDuration>
    [[nodiscard]] constexpr TargetDuration to() const noexcept {
        return std::chrono::duration_cast<TargetDuration>(to_chrono());
    }
    
    /**
     * @brief Factory method to create a Duration from days
     * @param days Number of days
     * @return A Duration representing the specified number of days
     */
    [[nodiscard]] static constexpr Duration from_days(rep days) noexcept {
        return Duration(days * 86400 * 1000000000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from hours
     * @param hours Number of hours
     * @return A Duration representing the specified number of hours
     */
    [[nodiscard]] static constexpr Duration from_hours(rep hours) noexcept {
        return Duration(hours * 3600 * 1000000000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from minutes
     * @param minutes Number of minutes
     * @return A Duration representing the specified number of minutes
     */
    [[nodiscard]] static constexpr Duration from_minutes(rep minutes) noexcept {
        return Duration(minutes * 60 * 1000000000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from seconds
     * @param seconds Number of seconds
     * @return A Duration representing the specified number of seconds
     */
    [[nodiscard]] static constexpr Duration from_seconds(rep seconds) noexcept {
        return Duration(seconds * 1000000000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from milliseconds
     * @param ms Number of milliseconds
     * @return A Duration representing the specified number of milliseconds
     */
    [[nodiscard]] static constexpr Duration from_milliseconds(rep ms) noexcept {
        return Duration(ms * 1000000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from microseconds
     * @param us Number of microseconds
     * @return A Duration representing the specified number of microseconds
     */
    [[nodiscard]] static constexpr Duration from_microseconds(rep us) noexcept {
        return Duration(us * 1000LL);
    }
    
    /**
     * @brief Factory method to create a Duration from nanoseconds
     * @param ns Number of nanoseconds
     * @return A Duration representing the specified number of nanoseconds
     */
    [[nodiscard]] static constexpr Duration from_nanoseconds(rep ns) noexcept {
        return Duration(ns);
    }
    
    // Value accessors
    
    /**
     * @brief Gets the duration in days
     * @return Number of whole days
     */
    [[nodiscard]] constexpr rep days() const noexcept {
        return _duration / (86400 * 1000000000LL);
    }
    
    /**
     * @brief Gets the duration in days with fractional precision
     * @return Number of days with decimal point
     */
    [[nodiscard]] constexpr double days_float() const noexcept {
        return static_cast<double>(_duration) / (86400.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the duration in hours
     * @return Number of whole hours
     */
    [[nodiscard]] constexpr rep hours() const noexcept {
        return _duration / (3600 * 1000000000LL);
    }
    
    /**
     * @brief Gets the duration in hours with fractional precision
     * @return Number of hours with decimal point
     */
    [[nodiscard]] constexpr double hours_float() const noexcept {
        return static_cast<double>(_duration) / (3600.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the duration in minutes
     * @return Number of whole minutes
     */
    [[nodiscard]] constexpr rep minutes() const noexcept {
        return _duration / (60 * 1000000000LL);
    }
    
    /**
     * @brief Gets the duration in minutes with fractional precision
     * @return Number of minutes with decimal point
     */
    [[nodiscard]] constexpr double minutes_float() const noexcept {
        return static_cast<double>(_duration) / (60.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the duration in seconds
     * @return Number of whole seconds
     */
    [[nodiscard]] constexpr rep seconds() const noexcept {
        return _duration / 1000000000LL;
    }
    
    /**
     * @brief Gets the duration in seconds with fractional precision
     * @return Number of seconds with decimal point
     */
    [[nodiscard]] constexpr double seconds_float() const noexcept {
        return static_cast<double>(_duration) / 1000000000.0;
    }
    
    /**
     * @brief Gets the duration in milliseconds
     * @return Number of whole milliseconds
     */
    [[nodiscard]] constexpr rep milliseconds() const noexcept {
        return _duration / 1000000LL;
    }
    
    /**
     * @brief Gets the duration in milliseconds with fractional precision
     * @return Number of milliseconds with decimal point
     */
    [[nodiscard]] constexpr double milliseconds_float() const noexcept {
        return static_cast<double>(_duration) / 1000000.0;
    }
    
    /**
     * @brief Gets the duration in microseconds
     * @return Number of whole microseconds
     */
    [[nodiscard]] constexpr rep microseconds() const noexcept {
        return _duration / 1000LL;
    }
    
    /**
     * @brief Gets the duration in microseconds with fractional precision
     * @return Number of microseconds with decimal point
     */
    [[nodiscard]] constexpr double microseconds_float() const noexcept {
        return static_cast<double>(_duration) / 1000.0;
    }
    
    /**
     * @brief Gets the duration in nanoseconds
     * @return Number of nanoseconds
     */
    [[nodiscard]] constexpr rep nanoseconds() const noexcept {
        return _duration;
    }
    
    /**
     * @brief Gets the duration in nanoseconds with double precision
     * @return Number of nanoseconds as double
     */
    [[nodiscard]] constexpr double nanoseconds_float() const noexcept {
        return static_cast<double>(_duration);
    }
    
    /**
     * @brief Gets the total duration in nanoseconds
     * @return Duration in nanoseconds
     */
    [[nodiscard]] constexpr rep count() const noexcept {
        return _duration;
    }
    
    // Unary operators
    constexpr Duration operator+() const noexcept { return *this; }
    constexpr Duration operator-() const noexcept { return Duration(-_duration); }
    
    // Compound assignment operators
    constexpr Duration& operator+=(const Duration& other) noexcept {
        _duration += other._duration;
        return *this;
    }
    
    constexpr Duration& operator-=(const Duration& other) noexcept {
        _duration -= other._duration;
        return *this;
    }
    
    constexpr Duration& operator*=(rep multiplier) noexcept {
        _duration *= multiplier;
        return *this;
    }
    
    constexpr Duration& operator/=(rep divisor) noexcept {
        _duration /= divisor;
        return *this;
    }
    
    constexpr Duration& operator%=(const Duration& other) noexcept {
        _duration %= other._duration;
        return *this;
    }
    
    // Comparison operators
    constexpr bool operator==(const Duration& rhs) const noexcept {
        return count() == rhs.count();
    }

    constexpr bool operator!=(const Duration& rhs) const noexcept {
        return count() != rhs.count();
    }

    constexpr bool operator<(const Duration& rhs) const noexcept {
        return count() < rhs.count();
    }

    constexpr bool operator<=(const Duration& rhs) const noexcept {
        return count() <= rhs.count();
    }

    constexpr bool operator>(const Duration& rhs) const noexcept {
        return count() > rhs.count();
    }

    constexpr bool operator>=(const Duration& rhs) const noexcept {
        return count() >= rhs.count();
    }
    
private:
    rep _duration{0}; ///< Duration in nanoseconds
};

// Binary arithmetic operators
constexpr Duration operator+(const Duration& lhs, const Duration& rhs) noexcept {
    return Duration(lhs.count() + rhs.count());
}

constexpr Duration operator-(const Duration& lhs, const Duration& rhs) noexcept {
    return Duration(lhs.count() - rhs.count());
}

constexpr Duration operator*(const Duration& lhs, Duration::rep rhs) noexcept {
    return Duration(lhs.count() * rhs);
}

constexpr Duration operator*(Duration::rep lhs, const Duration& rhs) noexcept {
    return Duration(lhs * rhs.count());
}

constexpr Duration operator/(const Duration& lhs, Duration::rep rhs) noexcept {
    return Duration(lhs.count() / rhs);
}

constexpr Duration::rep operator/(const Duration& lhs, const Duration& rhs) noexcept {
    return lhs.count() / rhs.count();
}

constexpr Duration operator%(const Duration& lhs, Duration::rep rhs) noexcept {
    return Duration(lhs.count() % rhs);
}

constexpr Duration operator%(const Duration& lhs, const Duration& rhs) noexcept {
    return Duration(lhs.count() % rhs.count());
}

// Literal operators for convenient duration creation
namespace literals {
    constexpr Duration operator""_d(unsigned long long days) noexcept {
        return Duration::from_days(days);
    }
    
    constexpr Duration operator""_h(unsigned long long hours) noexcept {
        return Duration::from_hours(hours);
    }
    
    constexpr Duration operator""_min(unsigned long long minutes) noexcept {
        return Duration::from_minutes(minutes);
    }
    
    constexpr Duration operator""_s(unsigned long long seconds) noexcept {
        return Duration::from_seconds(seconds);
    }
    
    constexpr Duration operator""_ms(unsigned long long milliseconds) noexcept {
        return Duration::from_milliseconds(milliseconds);
    }
    
    constexpr Duration operator""_us(unsigned long long microseconds) noexcept {
        return Duration::from_microseconds(microseconds);
    }
    
    constexpr Duration operator""_ns(unsigned long long nanoseconds) noexcept {
        return Duration::from_nanoseconds(nanoseconds);
    }
}

// For backward compatibility
using Timespan = Duration;

/**
 * @class TimePoint
 * @brief Represents a point in time with nanosecond precision
 *
 * TimePoint provides a platform-independent way to represent moments in time
 * with high precision. It supports arithmetic operations with Duration objects
 * and provides conversions to various time units and formats.
 */
class TimePoint {
public:
    /// Type used for nanosecond representation
    using rep = uint64_t;
    
    /// Underlying std::chrono time point type
    using chrono_time_point = std::chrono::time_point<std::chrono::system_clock, 
                                                      std::chrono::duration<rep, std::nano>>;
                                                      
    /// Represents the epoch (1970-01-01 00:00:00 UTC)
    static constexpr TimePoint epoch() noexcept { return TimePoint(0); }
    
    /**
     * @brief Default constructor, initializes to epoch
     */
    constexpr TimePoint() noexcept = default;
    
    /**
     * @brief Constructs a time point with specified time since epoch
     * @param nanoseconds Time in nanoseconds since epoch
     */
    constexpr explicit TimePoint(rep nanoseconds) noexcept
        : _time_since_epoch(nanoseconds) {}
        
    /**
     * @brief Constructs a time point from std::chrono::time_point
     * @tparam Clock The clock type
     * @tparam Duration The duration type
     * @param time_point A std::chrono time point
     */
    template <typename Clock, typename ChronoDuration>
    explicit TimePoint(const std::chrono::time_point<Clock, ChronoDuration>& time_point) noexcept
        : _time_since_epoch(std::chrono::duration_cast<std::chrono::nanoseconds>(
              time_point.time_since_epoch()).count()) {}
              
    // Standard special member functions
    constexpr TimePoint(const TimePoint&) noexcept = default;
    constexpr TimePoint(TimePoint&&) noexcept = default;
    constexpr TimePoint& operator=(const TimePoint&) noexcept = default;
    constexpr TimePoint& operator=(TimePoint&&) noexcept = default;
    ~TimePoint() noexcept = default;
    
    /**
     * @brief Gets current system time
     * @return TimePoint representing current system time
     */
    static TimePoint now() noexcept {
        // Use the combination of system clock (for absolute time) and 
        // steady clock (for monotonic time between calls)
        static const auto system_start = std::chrono::system_clock::now();
        static const auto steady_start = std::chrono::steady_clock::now();
        
        auto steady_now = std::chrono::steady_clock::now();
        auto delta = steady_now - steady_start;
        auto result = system_start + delta;
        
        return TimePoint(result);
    }
    
    /**
     * @brief Converts to std::chrono::time_point
     * @return Equivalent std::chrono::time_point
     */
    [[nodiscard]] chrono_time_point to_chrono() const noexcept {
        return std::chrono::time_point<std::chrono::system_clock>() + 
               std::chrono::nanoseconds(_time_since_epoch);
    }
    
    /**
     * @brief Converts to any std::chrono::time_point
     * @tparam Clock The target clock type
     * @tparam Duration The target duration type
     * @return Converted time point
     */
    template <typename Clock, typename ChronoDuration = typename Clock::duration>
    [[nodiscard]] std::chrono::time_point<Clock, ChronoDuration> 
    to() const noexcept {
        using target_tp = std::chrono::time_point<Clock, ChronoDuration>;
        return std::chrono::time_point_cast<ChronoDuration>(
            target_tp(std::chrono::duration_cast<ChronoDuration>(
                std::chrono::nanoseconds(_time_since_epoch))));
    }
    
    /**
     * @brief Factory method to create a TimePoint from days since epoch
     * @param days Number of days since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_days(int64_t days) noexcept {
        return TimePoint(days * 86400ULL * 1000000000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from hours since epoch
     * @param hours Number of hours since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_hours(int64_t hours) noexcept {
        return TimePoint(hours * 3600ULL * 1000000000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from minutes since epoch
     * @param minutes Number of minutes since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_minutes(int64_t minutes) noexcept {
        return TimePoint(minutes * 60ULL * 1000000000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from seconds since epoch
     * @param seconds Number of seconds since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_seconds(int64_t seconds) noexcept {
        return TimePoint(seconds * 1000000000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from milliseconds since epoch
     * @param ms Number of milliseconds since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_milliseconds(int64_t ms) noexcept {
        return TimePoint(ms * 1000000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from microseconds since epoch
     * @param us Number of microseconds since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_microseconds(int64_t us) noexcept {
        return TimePoint(us * 1000ULL);
    }
    
    /**
     * @brief Factory method to create a TimePoint from nanoseconds since epoch
     * @param ns Number of nanoseconds since epoch
     * @return A TimePoint at the specified time
     */
    [[nodiscard]] static constexpr TimePoint from_nanoseconds(int64_t ns) noexcept {
        return TimePoint(ns);
    }
    
    /**
     * @brief Creates a TimePoint from ISO8601 string
     * @param iso8601 ISO8601 formatted date-time string
     * @return Optional TimePoint, empty if parsing failed
     */
    [[nodiscard]] static std::optional<TimePoint> from_iso8601(std::string_view iso8601) noexcept {
        try {
            std::tm tm = {};
            std::string str_iso8601(iso8601);
            std::istringstream iss(str_iso8601);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (iss.fail()) {
                return std::nullopt;
            }
            const auto time_since_epoch = std::mktime(&tm);
            if (time_since_epoch == -1) {
                return std::nullopt;
            }
            return TimePoint(static_cast<rep>(time_since_epoch) * 1000000000ULL);
        } catch (...) {
            return std::nullopt;
        }
    }
    
    /**
     * @brief Factory method to parse a string into a TimePoint
     * @param time_string Time string
     * @param format Format string (strptime/std::get_time compatible)
     * @return Optional TimePoint, empty if parsing failed
     */
    [[nodiscard]] static std::optional<TimePoint> parse(
        std::string_view time_string, std::string_view format) noexcept {
        try {
            std::tm tm = {};
            std::string str_time_string(time_string);
            std::istringstream iss(str_time_string);
            iss >> std::get_time(&tm, format.data());
            if (iss.fail()) {
                return std::nullopt;
            }
            const auto time_since_epoch = std::mktime(&tm);
            if (time_since_epoch == -1) {
                return std::nullopt;
            }
            return TimePoint(static_cast<rep>(time_since_epoch) * 1000000000ULL);
        } catch (...) {
            return std::nullopt;
        }
    }
    
    // Value accessors
    
    /**
     * @brief Gets the time in days since epoch
     * @return Number of whole days
     */
    [[nodiscard]] constexpr rep days() const noexcept {
        return _time_since_epoch / (86400ULL * 1000000000ULL);
    }
    
    /**
     * @brief Gets the time in days since epoch with fractional precision
     * @return Number of days with decimal point
     */
    [[nodiscard]] constexpr double days_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / (86400.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the time in hours since epoch
     * @return Number of whole hours
     */
    [[nodiscard]] constexpr rep hours() const noexcept {
        return _time_since_epoch / (3600ULL * 1000000000ULL);
    }
    
    /**
     * @brief Gets the time in hours since epoch with fractional precision
     * @return Number of hours with decimal point
     */
    [[nodiscard]] constexpr double hours_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / (3600.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the time in minutes since epoch
     * @return Number of whole minutes
     */
    [[nodiscard]] constexpr rep minutes() const noexcept {
        return _time_since_epoch / (60ULL * 1000000000ULL);
    }
    
    /**
     * @brief Gets the time in minutes since epoch with fractional precision
     * @return Number of minutes with decimal point
     */
    [[nodiscard]] constexpr double minutes_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / (60.0 * 1000000000.0);
    }
    
    /**
     * @brief Gets the time in seconds since epoch
     * @return Number of whole seconds
     */
    [[nodiscard]] constexpr rep seconds() const noexcept {
        return _time_since_epoch / 1000000000ULL;
    }
    
    /**
     * @brief Gets the time in seconds since epoch with fractional precision
     * @return Number of seconds with decimal point
     */
    [[nodiscard]] constexpr double seconds_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / 1000000000.0;
    }
    
    /**
     * @brief Gets the time in milliseconds since epoch
     * @return Number of whole milliseconds
     */
    [[nodiscard]] constexpr rep milliseconds() const noexcept {
        return _time_since_epoch / 1000000ULL;
    }
    
    /**
     * @brief Gets the time in milliseconds since epoch with fractional precision
     * @return Number of milliseconds with decimal point
     */
    [[nodiscard]] constexpr double milliseconds_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / 1000000.0;
    }
    
    /**
     * @brief Gets the time in microseconds since epoch
     * @return Number of whole microseconds
     */
    [[nodiscard]] constexpr rep microseconds() const noexcept {
        return _time_since_epoch / 1000ULL;
    }
    
    /**
     * @brief Gets the time in microseconds since epoch with fractional precision
     * @return Number of microseconds with decimal point
     */
    [[nodiscard]] constexpr double microseconds_float() const noexcept {
        return static_cast<double>(_time_since_epoch) / 1000.0;
    }
    
    /**
     * @brief Gets the time in nanoseconds since epoch
     * @return Number of nanoseconds
     */
    [[nodiscard]] constexpr rep nanoseconds() const noexcept {
        return _time_since_epoch;
    }
    
    /**
     * @brief Gets the time in nanoseconds since epoch with double precision
     * @return Number of nanoseconds as double
     */
    [[nodiscard]] constexpr double nanoseconds_float() const noexcept {
        return static_cast<double>(_time_since_epoch);
    }
    
    /**
     * @brief Gets the duration since epoch
     * @return Duration object representing time since epoch
     */
    [[nodiscard]] Duration time_since_epoch() const noexcept {
        return Duration(static_cast<Duration::rep>(_time_since_epoch));
    }
    
    /**
     * @brief Gets the total time in nanoseconds since epoch
     * @return Time in nanoseconds
     */
    [[nodiscard]] constexpr rep count() const noexcept {
        return _time_since_epoch;
    }
    
    /**
     * @brief Formats the time point as a string
     * @param format Format string (strftime compatible)
     * @return Formatted string representation
     */
    [[nodiscard]] std::string format(std::string_view format) const {
        const auto time_t_value = static_cast<std::time_t>(seconds());
        std::tm tm = {};
        
#if defined(_MSC_VER)
        gmtime_s(&tm, &time_t_value);
#else
        tm = *std::gmtime(&time_t_value);
#endif
        
        std::array<char, 128> buffer{};
        const auto result = std::strftime(buffer.data(), buffer.size(), format.data(), &tm);
        
        if (result == 0) {
            return {};
        }
        
        return std::string(buffer.data(), result);
    }
    
    /**
     * @brief Converts to ISO8601 string
     * @return ISO8601 formatted date-time string
     */
    [[nodiscard]] std::string to_iso8601() const {
        return format("%Y-%m-%dT%H:%M:%SZ");
    }
    
    /**
     * @brief Reads CPU timestamp counter
     * @return Raw TSC value (platform dependent)
     */
    static uint64_t read_tsc() noexcept {
#if defined(_MSC_VER)
        return __rdtsc();
#elif defined(__i386__)
        uint64_t x;
        __asm__ volatile(".byte 0x0f, 0x31" : "=A"(x));
        return x;
#elif defined(__x86_64__)
        unsigned hi, lo;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)lo) | (((uint64_t)hi) << 32ULL);
#else
        // Fallback to high-resolution clock on platforms without rdtsc
        const auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
#endif
    }
    
    /**
     * @brief Adds a duration to this time point
     * @param duration Duration to add
     * @return Reference to this time point after addition
     */
    TimePoint& operator+=(const Duration& duration) noexcept {
        _time_since_epoch += static_cast<rep>(duration.count());
        return *this;
    }
    
    /**
     * @brief Subtracts a duration from this time point
     * @param duration Duration to subtract
     * @return Reference to this time point after subtraction
     */
    TimePoint& operator-=(const Duration& duration) noexcept {
        _time_since_epoch -= static_cast<rep>(duration.count());
        return *this;
    }

    /**
     * @brief Gets the current time in nanoseconds since epoch
     * @return Nanoseconds since epoch
     */
    static uint64_t nano() noexcept {
        return now().count();
    }

protected:
    rep _time_since_epoch{0}; ///< Time in nanoseconds since epoch
};

// Binary arithmetic operators
inline TimePoint operator+(const TimePoint& lhs, const Duration& rhs) noexcept {
    TimePoint result(lhs);
    result += rhs;
    return result;
}

inline TimePoint operator+(const Duration& lhs, const TimePoint& rhs) noexcept {
    return rhs + lhs;
}

inline TimePoint operator-(const TimePoint& lhs, const Duration& rhs) noexcept {
    TimePoint result(lhs);
    result -= rhs;
    return result;
}

inline Duration operator-(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return Duration(static_cast<Duration::rep>(lhs.count() - rhs.count()));
}

// Comparison operators
constexpr bool operator==(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() == rhs.count();
}

constexpr bool operator!=(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() != rhs.count();
}

constexpr bool operator<(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() < rhs.count();
}

constexpr bool operator<=(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() <= rhs.count();
}

constexpr bool operator>(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() > rhs.count();
}

constexpr bool operator>=(const TimePoint& lhs, const TimePoint& rhs) noexcept {
    return lhs.count() >= rhs.count();
}

/**
 * @class UtcTimePoint
 * @brief Represents a UTC time point with nanosecond precision
 *
 * Extends TimePoint to specifically represent times in Coordinated Universal Time (UTC).
 */
class UtcTimePoint : public TimePoint {
public:
    using TimePoint::TimePoint;
    
    /**
     * @brief Default constructor, initializes to current UTC time
     */
    UtcTimePoint() : TimePoint(TimePoint::now()) {}
    
    /**
     * @brief Gets current UTC time
     * @return UtcTimePoint representing current UTC time
     */
    static UtcTimePoint now() noexcept {
        auto tp = TimePoint::now();
        UtcTimePoint result;
        // Convert timepoint to epoch value and construct UtcTimePoint with it
        result = UtcTimePoint(tp.count());
        return result;
    }
};

/**
 * @class LocalTimePoint
 * @brief Represents a local time point with nanosecond precision
 *
 * Extends TimePoint to specifically represent times in local timezone.
 */
class LocalTimePoint : public TimePoint {
public:
    using TimePoint::TimePoint;
    
    /**
     * @brief Default constructor, initializes to current local time
     */
    LocalTimePoint() : TimePoint(TimePoint::now()) {}
    
    /**
     * @brief Gets current local time
     * @return LocalTimePoint representing current local time
     */
    static LocalTimePoint now() noexcept {
        auto tp = TimePoint::now();
        LocalTimePoint result;
        // Convert timepoint to epoch value and construct LocalTimePoint with it
        result = LocalTimePoint(tp.count());
        return result;
    }
    
    /**
     * @brief Formats the time point as a local time string
     * @param format Format string (strftime compatible)
     * @return Formatted local time string
     */
    [[nodiscard]] std::string format_local(std::string_view format) const {
        const auto time_t_value = static_cast<std::time_t>(seconds());
        std::tm tm = {};
        
#if defined(_MSC_VER)
        localtime_s(&tm, &time_t_value);
#else
        tm = *std::localtime(&time_t_value);
#endif
        
        std::array<char, 128> buffer{};
        const auto result = std::strftime(buffer.data(), buffer.size(), format.data(), &tm);
        
        if (result == 0) {
            return {};
        }
        
        return std::string(buffer.data(), result);
    }
};

/**
 * @class HighResTimePoint
 * @brief Represents a high-resolution time point
 *
 * Extends TimePoint to specifically represent high-precision times from
 * the system's high-resolution timer.
 */
class HighResTimePoint : public TimePoint {
public:
    using TimePoint::TimePoint;
    
    /**
     * @brief Default constructor, initializes to current high-resolution time
     */
    HighResTimePoint() : TimePoint(TimePoint::now()) {}
    
    /**
     * @brief Gets current high-resolution time
     * @return HighResTimePoint representing current high-resolution time
     */
    static HighResTimePoint now() noexcept {
        auto tp = TimePoint::now();
        HighResTimePoint result;
        // Convert timepoint to epoch value and construct HighResTimePoint with it
        result = HighResTimePoint(tp.count());
        return result;
    }
};

/**
 * @class TscTimePoint
 * @brief Represents a time point based on CPU's timestamp counter
 *
 * Extends TimePoint to specifically represent times based on the CPU's
 * timestamp counter, which provides very high precision but may
 * vary between CPU cores.
 */
class TscTimePoint : public TimePoint {
public:
    using TimePoint::TimePoint;
    
    /**
     * @brief Default constructor, initializes to current TSC time
     */
    TscTimePoint() : TimePoint(TimePoint::read_tsc()) {}
    
    /**
     * @brief Gets current TSC time
     * @return TscTimePoint based on CPU timestamp counter
     */
    static TscTimePoint now() noexcept {
        return TscTimePoint(TimePoint::read_tsc());
    }
};

// For backward compatibility
using Timestamp = TimePoint;
using UtcTimestamp = UtcTimePoint;
using LocalTimestamp = LocalTimePoint;
using NanoTimestamp = HighResTimePoint;
using RdtsTimestamp = TscTimePoint;

/**
 * @class ScopedTimer
 * @brief Utility for measuring code block execution time
 *
 * Measures elapsed time between construction and destruction,
 * optionally invoking a callback with the measured duration.
 */
class ScopedTimer {
public:
    using TimerCallback = std::function<void(Duration)>;
    
    /**
     * @brief Constructs a timer with a callback
     * @param callback Function to call with measured duration when timer is destroyed
     */
    explicit ScopedTimer(TimerCallback callback)
        : _start_time(HighResTimePoint::now())
        , _callback(std::move(callback))
        , _active(true) {}
        
    /**
     * @brief Destructor that invokes callback with elapsed time
     */
    ~ScopedTimer() {
        stop();
    }
    
    /**
     * @brief Stops the timer and invokes callback if active
     * @return Measured duration
     */
    Duration stop() {
        if (!_active) {
            return _elapsed;
        }
        
        _active = false;
        _elapsed = HighResTimePoint::now() - _start_time;
        
        if (_callback) {
            _callback(_elapsed);
        }
        
        return _elapsed;
    }
    
    /**
     * @brief Restarts the timer
     */
    void restart() {
        _start_time = HighResTimePoint::now();
        _active = true;
    }
    
    /**
     * @brief Gets elapsed time without stopping timer
     * @return Current elapsed duration
     */
    Duration elapsed() const {
        if (!_active) {
            return _elapsed;
        }
        
        return HighResTimePoint::now() - _start_time;
    }
    
    // Deleted copy/move operations to prevent double-stopping
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;
    
private:
    HighResTimePoint _start_time;
    TimerCallback _callback;
    Duration _elapsed{0};
    bool _active;
};

/**
 * @class LogTimer
 * @brief Utility for logging execution time of code blocks
 *
 * Creates a timer that logs the elapsed time when it goes out of scope.
 * Useful for performance measurements and debugging.
 */
class LogTimer {
public:
    /**
     * @brief Constructs a timer with a descriptive reason
     * @param reason Description of what is being timed
     */
    explicit LogTimer(std::string reason)
        : _reason(std::move(reason))
        , _timer([this](Duration duration) {
              qb::io::cout() << _reason << ": " << duration.microseconds() << "us" << std::endl;
          }) {}
          
    /**
     * @brief Gets elapsed time without stopping timer
     * @return Current elapsed duration
     */
    Duration elapsed() const {
        return _timer.elapsed();
    }
    
private:
    std::string _reason;
    ScopedTimer _timer;
};

} // namespace qb

// Stream operator
inline std::ostream& operator<<(std::ostream& os, const qb::TimePoint& tp) {
    return os << tp.to_iso8601();
}

inline std::ostream& operator<<(std::ostream& os, const qb::Duration& d) {
    auto seconds = d.seconds();
    auto nanoseconds = d.nanoseconds() % 1000000000LL;
    
    if (nanoseconds == 0) {
        return os << seconds << "s";
    } else if (nanoseconds % 1000000LL == 0) {
        return os << seconds << "s " << (nanoseconds / 1000000LL) << "ms";
    } else if (nanoseconds % 1000LL == 0) {
        return os << seconds << "s " << (nanoseconds / 1000LL) << "us";
    } else {
        return os << seconds << "s " << nanoseconds << "ns";
    }
}

// Hash support for unordered containers
namespace std {
    template<>
    struct hash<qb::Duration> {
        size_t operator()(const qb::Duration& duration) const noexcept {
            return std::hash<int64_t>{}(duration.count());
        }
    };
    
    template<>
    struct hash<qb::TimePoint> {
        size_t operator()(const qb::TimePoint& time_point) const noexcept {
            return std::hash<uint64_t>{}(time_point.count());
        }
    };
}

// std::formatter support (C++20)
#if __cplusplus >= 202002L
namespace std {
    template<>
    struct formatter<qb::TimePoint> {
        constexpr auto parse(format_parse_context& ctx) {
            return ctx.begin();
        }
        
        auto format(const qb::TimePoint& tp, format_context& ctx) const {
            return format_to(ctx.out(), "{}", tp.to_iso8601());
        }
    };
    
    template<>
    struct formatter<qb::Duration> {
        constexpr auto parse(format_parse_context& ctx) {
            return ctx.begin();
        }
        
        auto format(const qb::Duration& d, format_context& ctx) const {
            auto seconds = d.seconds();
            auto nanoseconds = d.nanoseconds() % 1000000000LL;
            
            if (nanoseconds == 0) {
                return format_to(ctx.out(), "{}s", seconds);
            } else if (nanoseconds % 1000000LL == 0) {
                return format_to(ctx.out(), "{}s {}ms", seconds, nanoseconds / 1000000LL);
            } else if (nanoseconds % 1000LL == 0) {
                return format_to(ctx.out(), "{}s {}us", seconds, nanoseconds / 1000LL);
            } else {
                return format_to(ctx.out(), "{}s {}ns", seconds, nanoseconds);
            }
        }
    };
}
#endif

#endif // FEATURES_TIMESTAMP_H