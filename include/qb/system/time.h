/**
 * @file qb/system/time.h
 * @brief Canonical time vocabulary for the QB framework (std::chrono based).
 *
 * Single source of truth for time across qb and its modules. The model is
 * deliberately minimal and built entirely on `std::chrono`:
 *
 *  - `qb::duration`  — a span. Signed, nanosecond resolution (`int64`). Accepts
 *                      any finer-or-equal `std::chrono::duration` implicitly
 *                      (`30s`, `100ms`, `5us`), and **rejects a raw integer** —
 *                      so unit-confusion (seconds vs milliseconds) cannot compile.
 *  - `qb::mono_time` — a monotonic instant (`steady_clock`). Use for deadlines,
 *                      timers, the event-loop "now", latency and RTT. Immune to
 *                      wall-clock adjustments (NTP / DST).
 *  - `qb::wall_time` — a wall-clock instant (`system_clock`). Use for dates,
 *                      expiry, JWT exp/nbf, TLS validity, logs and wire formats.
 *
 * The two instant types are distinct on purpose: subtracting a wall instant from
 * a monotonic one does not compile, which removes a whole class of "timeout
 * fired early because the clock stepped" bugs.
 *
 * Raw scalars (`double` seconds, `int`/`uint64` milliseconds) and the C date
 * library live ONLY behind the named seams in this header (`detail::to_ev_*`,
 * `format_utc`/`parse_utc`) and the per-protocol wire codecs — never in a public
 * signature elsewhere.
 *
 * Toolchain note: on this build (Apple clang / libc++) `std::format` supports
 * chrono but `std::chrono::from_stream`/`parse` and the time-zone database are
 * NOT available — hence formatting uses `std::format`/`strftime` and parsing uses
 * `std::get_time` + `qb::safe_timegm`, all in UTC with no tzdb dependency.
 *
 * UTC calendar conversion (`safe_gmtime`/`safe_timegm`) is done with pure integer
 * arithmetic rather than the C library: `gmtime_s`/`_mkgmtime` reject a negative
 * `time_t` on Windows, so every instant before 1970-01-01 would diverge from
 * POSIX. The integer path is exact and identical on every platform, and the
 * caller-owned `std::tm` makes it thread-safe (unlike `std::gmtime`).
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
 * @ingroup Time
 */

#ifndef QB_SYSTEM_TIME_H
#define QB_SYSTEM_TIME_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace qb {

/**
 * @defgroup Time Time
 * @brief Canonical std::chrono time vocabulary (durations, instants, helpers).
 */

// ---------------------------------------------------------------------------
// Canonical types
// ---------------------------------------------------------------------------

/// Canonical span: signed, nanosecond resolution. Public timeout / TTL /
/// interval parameters take this by value; it accepts any finer-or-equal
/// chrono literal implicitly and rejects bare integers.
using duration = std::chrono::nanoseconds;

/// Monotonic instant — deadlines, timers, event-loop "now", latency, RTT.
using mono_time = std::chrono::steady_clock::time_point;

/// Wall-clock instant — dates, expiry, JWT, TLS validity, logs, wire formats.
using wall_time = std::chrono::system_clock::time_point;

/// Current monotonic instant.
[[nodiscard]] inline mono_time
mono_now() noexcept {
    return std::chrono::steady_clock::now();
}

/// Current wall-clock instant.
[[nodiscard]] inline wall_time
wall_now() noexcept {
    return std::chrono::system_clock::now();
}

/// Bring the std::chrono literals (`30s`, `100ms`, `5us`, ...) into `qb` so
/// call sites can write them without an extra `using`.
inline namespace time_literals {
using namespace std::chrono_literals;
} // namespace time_literals

// ---------------------------------------------------------------------------
// Unix-epoch scalar extraction (wire / logging boundaries)
// ---------------------------------------------------------------------------

/// Whole seconds since the Unix epoch.
[[nodiscard]] inline std::int64_t
unix_seconds(wall_time tp) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

/// Whole milliseconds since the Unix epoch.
[[nodiscard]] inline std::int64_t
unix_millis(wall_time tp) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

/// Whole microseconds since the Unix epoch.
[[nodiscard]] inline std::int64_t
unix_micros(wall_time tp) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

/// Whole nanoseconds since the Unix epoch.
[[nodiscard]] inline std::int64_t
unix_nanos(wall_time tp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

/// Build a wall instant from whole seconds since the Unix epoch.
[[nodiscard]] inline wall_time
wall_from_unix_seconds(std::int64_t s) noexcept {
    return wall_time{std::chrono::seconds{s}};
}

/// Build a wall instant from whole milliseconds since the Unix epoch.
[[nodiscard]] inline wall_time
wall_from_unix_millis(std::int64_t ms) noexcept {
    return wall_time{std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds{ms})};
}

/// Build a wall instant from whole nanoseconds since the Unix epoch.
[[nodiscard]] inline wall_time
wall_from_unix_nanos(std::int64_t ns) noexcept {
    return wall_time{std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{ns})};
}

// ---------------------------------------------------------------------------
// Portable UTC calendar conversions (thread-safe, valid for all time_t)
// ---------------------------------------------------------------------------
//
// std::gmtime / std::localtime return a pointer into a process-wide static tm
// (a data race across threads), and the C library's gmtime_s / _mkgmtime reject
// a *negative* time_t on Windows — so every instant before 1970-01-01 fails
// there while POSIX gmtime_r / timegm accept it. The helpers below take a
// caller-owned tm (thread-safe) and, for UTC, compute the calendar date with
// pure integer arithmetic (Howard Hinnant's civil algorithms) so they are exact
// for ALL time_t and identical on every platform. safe_localtime keeps the C
// library because local time genuinely needs the platform time-zone database.

namespace detail {

/// Days since 1970-01-01 for a proleptic-Gregorian civil date (`y` may be <= 0).
[[nodiscard]] constexpr std::int64_t
days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned     yoe = static_cast<unsigned>(y - era * 400);             // [0, 399]
    const unsigned     doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; // [0, 365]
    const unsigned     doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Broken-down civil date.
struct civil_date {
    std::int64_t year;
    unsigned     month; // [1, 12]
    unsigned     day;   // [1, 31]
};

/// Civil date from days since 1970-01-01 (`z` may be negative). Inverse of
/// days_from_civil.
[[nodiscard]] constexpr civil_date
civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned     doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
    const unsigned     yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const std::int64_t y   = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned     doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const unsigned     mp  = (5 * doy + 2) / 153;                     // [0, 11]
    const unsigned     d   = doy - (153 * mp + 2) / 5 + 1;            // [1, 31]
    const unsigned     m   = mp < 10 ? mp + 3 : mp - 9;               // [1, 12]
    return civil_date{y + (m <= 2), m, d};
}

} // namespace detail

/// Thread-safe, portable UTC breakdown of a `time_t` (replacement for gmtime_r /
/// gmtime_s). Pure integer math — valid for ALL time_t including negative
/// (pre-1970). Fills tm_year/mon/mday/hour/min/sec/wday/yday and tm_isdst=0.
/// Returns false only if the year would overflow the `int tm_year` field.
[[nodiscard]] inline bool
safe_gmtime(std::time_t t, std::tm &out) noexcept {
    const std::int64_t secs = static_cast<std::int64_t>(t);
    std::int64_t       days = secs / 86400;
    std::int64_t       rem  = secs % 86400;
    if (rem < 0) { // floor toward negative infinity
        rem += 86400;
        --days;
    }
    out.tm_hour = static_cast<int>(rem / 3600);
    out.tm_min  = static_cast<int>((rem % 3600) / 60);
    out.tm_sec  = static_cast<int>(rem % 60);

    std::int64_t wday = (days % 7 + 4) % 7; // 1970-01-01 was a Thursday (tm_wday == 4)
    if (wday < 0)
        wday += 7;
    out.tm_wday = static_cast<int>(wday);

    const detail::civil_date c = detail::civil_from_days(days);

    static constexpr int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool           leap  = (c.year % 4 == 0 && c.year % 100 != 0) || (c.year % 400 == 0);
    out.tm_yday                = cum[c.month - 1] + static_cast<int>(c.day) - 1 + ((c.month > 2 && leap) ? 1 : 0);

    const std::int64_t tm_year = c.year - 1900;
    if (tm_year < std::numeric_limits<int>::min() || tm_year > std::numeric_limits<int>::max())
        return false;
    out.tm_year  = static_cast<int>(tm_year);
    out.tm_mon   = static_cast<int>(c.month - 1);
    out.tm_mday  = static_cast<int>(c.day);
    out.tm_isdst = 0;
    return true;
}

/// Thread-safe, portable UTC `std::tm` -> `time_t` (replacement for timegm /
/// _mkgmtime). Inverse of safe_gmtime; exact for all dates including pre-1970.
[[nodiscard]] inline std::time_t
safe_timegm(const std::tm &in) noexcept {
    const std::int64_t days = detail::days_from_civil(static_cast<std::int64_t>(in.tm_year) + 1900, static_cast<unsigned>(in.tm_mon + 1),
                                                      static_cast<unsigned>(in.tm_mday));
    return static_cast<std::time_t>(days * 86400 + static_cast<std::int64_t>(in.tm_hour) * 3600 + static_cast<std::int64_t>(in.tm_min) * 60
                                    + static_cast<std::int64_t>(in.tm_sec));
}

/// Thread-safe LOCAL-time breakdown (keeps the platform time-zone database).
[[nodiscard]] inline bool
safe_localtime(std::time_t t, std::tm &out) noexcept {
#if defined(_WIN32)
    return ::localtime_s(&out, &t) == 0;
#else
    return ::localtime_r(&t, &out) != nullptr;
#endif
}

// ---------------------------------------------------------------------------
// UTC formatting / parsing (no time-zone database dependency)
// ---------------------------------------------------------------------------

/// Format a wall instant as UTC using a strftime-compatible format string.
/// Returns an empty string on failure.
[[nodiscard]] inline std::string
format_utc(wall_time tp, std::string_view fmt) {
    const auto t = static_cast<std::time_t>(unix_seconds(tp));
    std::tm    tm{};
    if (!safe_gmtime(t, tm))
        return {};
    std::array<char, 128> buf{};
    const std::string     f(fmt);
    const auto            n = std::strftime(buf.data(), buf.size(), f.c_str(), &tm);
    return std::string(buf.data(), n);
}

/// Format a wall instant as an ISO-8601 UTC string ("YYYY-MM-DDTHH:MM:SSZ").
[[nodiscard]] inline std::string
to_iso8601(wall_time tp) {
    return format_utc(tp, "%Y-%m-%dT%H:%M:%SZ");
}

/// Parse a UTC time string with a get_time-compatible format. The broken-down
/// time is interpreted as UTC (timegm / _mkgmtime), never local time. Returns
/// std::nullopt on any parse error.
[[nodiscard]] inline std::optional<wall_time>
parse_utc(std::string_view str, std::string_view fmt) noexcept {
    try {
        std::tm            tm{};
        const std::string  s(str);
        const std::string  f(fmt); // get_time needs a null-terminated format
        std::istringstream iss(s);
        iss >> std::get_time(&tm, f.c_str());
        if (iss.fail())
            return std::nullopt;
        // safe_timegm is exact for every date (no -1 sentinel) and handles
        // pre-1970 instants that _mkgmtime rejects on Windows.
        return wall_from_unix_seconds(static_cast<std::int64_t>(safe_timegm(tm)));
    } catch (...) {
        return std::nullopt;
    }
}

/// Parse an ISO-8601 UTC string ("YYYY-MM-DDTHH:MM:SSZ").
[[nodiscard]] inline std::optional<wall_time>
from_iso8601(std::string_view iso8601) noexcept {
    return parse_utc(iso8601, "%Y-%m-%dT%H:%M:%SZ");
}

// ---------------------------------------------------------------------------
// Calendar-date / time-of-day component helpers (no instant, no time zone)
// ---------------------------------------------------------------------------
//
// These operate on the raw components used by date/time wire formats: a count of
// whole days since the Unix epoch (a calendar date), microseconds since midnight
// (a time of day), and a UTC offset in seconds. They use the same exact-integer
// civil algorithms as safe_gmtime, so they are valid for every date including
// pre-1970. Wire codecs (e.g. PostgreSQL DATE/TIME/TIMETZ) should build on these
// rather than re-deriving the arithmetic.

/// Format a proleptic-Gregorian calendar date ("YYYY-MM-DD") from a count of
/// whole days since the Unix epoch (1970-01-01). Negative = before the epoch.
[[nodiscard]] inline std::string
format_date(std::int64_t days_since_epoch) {
    const detail::civil_date c = detail::civil_from_days(days_since_epoch);
    // Sized for the worst case the formatter can produce: a full 64-bit year
    // (up to 20 chars incl. sign) + two unsigned month/day fields + separators + NUL.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(c.year), c.month, c.day);
    return std::string(buf);
}

/// Parse a calendar date ("YYYY-MM-DD") to whole days since the Unix epoch.
/// Returns std::nullopt if the three integer fields cannot be read.
[[nodiscard]] inline std::optional<std::int64_t>
parse_date(std::string_view date) noexcept {
    int               y = 0, m = 0, d = 0;
    const std::string s(date);
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
        return std::nullopt;
    return detail::days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

/// Format a time of day ("HH:MM:SS" or "HH:MM:SS.ffffff") from microseconds since
/// midnight. The fractional part is emitted only when non-zero.
[[nodiscard]] inline std::string
format_time_of_day(std::int64_t micros_since_midnight) {
    const std::int64_t total_seconds = micros_since_midnight / 1000000;
    const int          hour          = static_cast<int>(total_seconds / 3600);
    const int          minute        = static_cast<int>((total_seconds % 3600) / 60);
    const int          second        = static_cast<int>(total_seconds % 60);
    const int          micros        = static_cast<int>(micros_since_midnight % 1000000);
    char               buf[32];
    if (micros > 0)
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", hour, minute, second, micros);
    else
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour, minute, second);
    return std::string(buf);
}

/// Parse a time of day ("HH:MM:SS" or "HH:MM:SS.ffffff") to microseconds since
/// midnight. Returns std::nullopt if at least HH:MM:SS cannot be read. The
/// fractional field is taken verbatim as microseconds (so a literal 6-digit
/// fraction round-trips with format_time_of_day).
[[nodiscard]] inline std::optional<std::int64_t>
parse_time_of_day(std::string_view tod) noexcept {
    int               hour = 0, minute = 0, second = 0, micros = 0;
    const std::string s(tod);
    if (std::sscanf(s.c_str(), "%d:%d:%d.%d", &hour, &minute, &second, &micros) < 3)
        return std::nullopt;
    return ((static_cast<std::int64_t>(hour) * 3600) + (static_cast<std::int64_t>(minute) * 60) + second) * 1000000LL + micros;
}

/// Format a UTC offset ("+HH:MM" / "-HH:MM") from a signed seconds-east value
/// (e.g. +7200 -> "+02:00", -18000 -> "-05:00").
[[nodiscard]] inline std::string
format_utc_offset(std::int32_t seconds_east) {
    const int abs_secs = seconds_east < 0 ? -seconds_east : seconds_east;
    const int hh       = abs_secs / 3600;
    const int mm       = (abs_secs % 3600) / 60;
    // Sign tracks the printed magnitude, not the raw value: a western offset smaller than
    // one minute rounds to 00:00, and "-00:00" is NOT a canonical UTC offset — in ISO 8601
    // / RFC 3339 it specifically means "offset unknown", semantically distinct from +00:00.
    const char sign = (seconds_east < 0 && (hh != 0 || mm != 0)) ? '-' : '+';
    // Sized for any int32 offset: sign + up to 6-digit hours + ':' + 2-digit min + NUL.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, hh, mm);
    return std::string(buf);
}

/// Parse a UTC offset to signed seconds east of UTC. Accepts the forms
/// PostgreSQL emits for timetz/timestamptz — "+HH", "±HH:MM", "±HH:MM:SS" — and
/// "Z"/"z" (zero). Inverse of format_utc_offset. Returns std::nullopt if the
/// leading sign and hour cannot be read.
[[nodiscard]] inline std::optional<std::int32_t>
parse_utc_offset(std::string_view off) noexcept {
    if (off.size() == 1 && (off[0] == 'Z' || off[0] == 'z'))
        return 0;
    if (off.empty() || (off[0] != '+' && off[0] != '-'))
        return std::nullopt;
    const int         sign = (off[0] == '-') ? -1 : 1;
    int               hour = 0, minute = 0, second = 0;
    const std::string s(off.substr(1));
    if (std::sscanf(s.c_str(), "%d:%d:%d", &hour, &minute, &second) < 1)
        return std::nullopt;
    return sign * (hour * 3600 + minute * 60 + second);
}

// ---------------------------------------------------------------------------
// Civil calendar / time-of-day value types
// ---------------------------------------------------------------------------
//
// These complete the vocabulary alongside the instant types: a `wall_time` is a
// point on the UTC timeline, whereas a `date` / `time_of_day` is a *civil* label
// with no inherent instant (a DATE has no time or zone; a TIME has no date). They
// are built on std::chrono's C++20 calendar (`sys_days`, `year_month_day`,
// `hh_mm_ss`) and the exact-integer helpers above, so they are valid for every
// date including pre-1970 and carry no time-zone-database dependency. Wire codecs
// (e.g. PostgreSQL DATE/TIME/TIMETZ/INTERVAL) map onto these instead of inventing
// their own calendar arithmetic.

/// A calendar date (proleptic Gregorian; no time, no time zone). Stored as whole
/// days since the Unix epoch via `std::chrono::sys_days`. Distinct from
/// `qb::wall_time`: a date is a civil label, not an instant.
class date {
    std::chrono::sys_days days_{};

public:
    constexpr date() noexcept = default;
    constexpr explicit date(std::chrono::sys_days d) noexcept
        : days_(d) {}

    /// From whole days since the Unix epoch (1970-01-01). Negative = before.
    [[nodiscard]] static constexpr date
    from_days_since_epoch(std::int64_t d) noexcept {
        return date{std::chrono::sys_days{std::chrono::days{d}}};
    }
    /// From a civil year/month/day (proleptic Gregorian; `y` may be <= 0).
    [[nodiscard]] static constexpr date
    from_ymd(std::int64_t y, unsigned m, unsigned d) noexcept {
        return from_days_since_epoch(detail::days_from_civil(y, m, d));
    }
    /// The UTC calendar date containing a wall instant (floored).
    [[nodiscard]] static date
    from_wall_time(wall_time tp) noexcept {
        const std::int64_t s = unix_seconds(tp);
        std::int64_t       d = s / 86400;
        if (s % 86400 < 0) // floor toward negative infinity
            --d;
        return from_days_since_epoch(d);
    }
    [[nodiscard]] static date
    today() noexcept {
        return from_wall_time(wall_now());
    }

    [[nodiscard]] constexpr std::chrono::sys_days
    to_sys_days() const noexcept {
        return days_;
    }
    [[nodiscard]] constexpr std::int64_t
    days_since_epoch() const noexcept {
        return days_.time_since_epoch().count();
    }
    [[nodiscard]] std::chrono::year_month_day
    year_month_day() const noexcept {
        return std::chrono::year_month_day{days_};
    }
    /// Midnight UTC of this date as a wall instant.
    [[nodiscard]] wall_time
    to_wall_time() const noexcept {
        return wall_from_unix_seconds(days_since_epoch() * 86400);
    }

    /// "YYYY-MM-DD" (UTC).
    [[nodiscard]] std::string
    to_string() const {
        return format_date(days_since_epoch());
    }
    /// Parse "YYYY-MM-DD"; std::nullopt on malformed input.
    [[nodiscard]] static std::optional<date>
    parse(std::string_view s) noexcept {
        if (auto d = parse_date(s))
            return from_days_since_epoch(*d);
        return std::nullopt;
    }

    constexpr date &
    operator+=(std::chrono::days n) noexcept {
        days_ += n;
        return *this;
    }
    constexpr date &
    operator-=(std::chrono::days n) noexcept {
        days_ -= n;
        return *this;
    }
    [[nodiscard]] friend constexpr date
    operator+(date a, std::chrono::days n) noexcept {
        return a += n;
    }
    [[nodiscard]] friend constexpr std::chrono::days
    operator-(date a, date b) noexcept {
        return a.days_ - b.days_;
    }
    [[nodiscard]] constexpr auto operator<=>(const date &) const noexcept = default;
};

/// A wall time within a day (no date, no time zone): microseconds since midnight,
/// normally in [0, 24h). Distinct from `qb::duration` (a span) and `qb::wall_time`.
class time_of_day {
    std::chrono::microseconds us_{};

public:
    constexpr time_of_day() noexcept = default;
    constexpr explicit time_of_day(std::chrono::microseconds us) noexcept
        : us_(us) {}

    [[nodiscard]] static constexpr time_of_day
    from_micros(std::int64_t us) noexcept {
        return time_of_day{std::chrono::microseconds{us}};
    }
    [[nodiscard]] static constexpr time_of_day
    from_hms(int h, int m, int s, int us = 0) noexcept {
        return from_micros(((static_cast<std::int64_t>(h) * 3600) + (static_cast<std::int64_t>(m) * 60) + s) * 1000000 + us);
    }

    [[nodiscard]] constexpr std::chrono::microseconds
    since_midnight() const noexcept {
        return us_;
    }
    [[nodiscard]] std::chrono::hh_mm_ss<std::chrono::microseconds>
    hms() const noexcept {
        return std::chrono::hh_mm_ss<std::chrono::microseconds>{us_};
    }

    /// "HH:MM:SS" or "HH:MM:SS.ffffff".
    [[nodiscard]] std::string
    to_string() const {
        return format_time_of_day(us_.count());
    }
    [[nodiscard]] static std::optional<time_of_day>
    parse(std::string_view s) noexcept {
        if (auto m = parse_time_of_day(s))
            return from_micros(*m);
        return std::nullopt;
    }
    [[nodiscard]] constexpr auto operator<=>(const time_of_day &) const noexcept = default;
};

/// A wall time within a day plus a fixed UTC offset, east-positive (+02:00 =
/// +7200s, matching `format_utc_offset`).
struct time_of_day_tz {
    time_of_day          tod{};
    std::chrono::seconds offset{}; ///< seconds EAST of UTC

    constexpr time_of_day_tz() noexcept = default;
    constexpr time_of_day_tz(time_of_day t, std::chrono::seconds off) noexcept
        : tod(t)
        , offset(off) {}
    [[nodiscard]] static constexpr time_of_day_tz
    from_hms_offset(int h, int m, int s, int us, int offset_secs_east) noexcept {
        return {time_of_day::from_hms(h, m, s, us), std::chrono::seconds{offset_secs_east}};
    }

    /// "HH:MM:SS[.ffffff]±HH:MM".
    [[nodiscard]] std::string
    to_string() const {
        return tod.to_string() + format_utc_offset(static_cast<std::int32_t>(offset.count()));
    }
    [[nodiscard]] constexpr auto operator<=>(const time_of_day_tz &) const noexcept = default;
};

/// A PostgreSQL-style calendar interval: months + days + sub-day microseconds kept
/// SEPARATE (a month is not a fixed number of days, a day not a fixed number of
/// hours under DST). Lossless, unlike folding into a single `qb::duration`.
struct calendar_interval {
    std::int32_t              months{};
    std::int32_t              days{};
    std::chrono::microseconds micros{};

    constexpr calendar_interval() noexcept = default;
    constexpr calendar_interval(std::int32_t mo, std::int32_t d, std::chrono::microseconds us) noexcept
        : months(mo)
        , days(d)
        , micros(us) {}

    /// Total span under the conventional fold used by PostgreSQL EXTRACT(EPOCH):
    /// a day = 24h, a whole year (12 months) = 365.25 days, a residual month = 30
    /// days. Lossy by nature (calendar units collapsed to a fixed span).
    [[nodiscard]] constexpr std::chrono::microseconds
    to_micros() const noexcept {
        constexpr std::int64_t USECS_PER_DAY  = 86400LL * 1000000;
        constexpr std::int64_t USECS_PER_YEAR = 31557600LL * 1000000; // 365.25 days
        return micros
               + std::chrono::microseconds{
                   static_cast<std::int64_t>(days) * USECS_PER_DAY + static_cast<std::int64_t>(months / 12) * USECS_PER_YEAR
                   + static_cast<std::int64_t>(months % 12) * 30 * USECS_PER_DAY
               };
    }

    /// A readable "[N mons] [N days] HH:MM:SS[.ffffff]" form (not bit-identical to
    /// PostgreSQL's interval_out, but unambiguous).
    [[nodiscard]] std::string
    to_string() const {
        std::string out;
        if (months)
            out += std::to_string(months) + (months == 1 || months == -1 ? " mon " : " mons ");
        if (days)
            out += std::to_string(days) + (days == 1 || days == -1 ? " day " : " days ");
        const std::int64_t us = micros.count();
        if (us < 0)
            out += '-';
        out += format_time_of_day(us < 0 ? -us : us);
        return out;
    }
    [[nodiscard]] constexpr auto operator<=>(const calendar_interval &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Raw CPU timestamp counter (performance instrumentation only — NOT a clock)
// ---------------------------------------------------------------------------

/// Reads the platform CPU timestamp counter. Monotonic per-core, very high
/// resolution, but uncalibrated and not comparable to wall/monotonic clocks.
/// Use only for micro-benchmark deltas on a single thread.
[[nodiscard]] inline std::uint64_t
tsc_ticks() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return __rdtsc();
#elif defined(__i386__)
    std::uint64_t x;
    __asm__ volatile(".byte 0x0f, 0x31" : "=A"(x));
    return x;
#elif defined(__x86_64__)
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(lo)) | (static_cast<std::uint64_t>(hi) << 32ULL);
#elif defined(__aarch64__)
    std::uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__arm__) && defined(__ARM_ARCH_7A__)
    std::uint64_t val;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(val));
    return val;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count());
#endif
}

// ---------------------------------------------------------------------------
// Scoped measurement helpers
// ---------------------------------------------------------------------------

/**
 * @class ScopedTimer
 * @brief Measures elapsed monotonic time between construction and stop()/dtor,
 *        invoking an optional callback with the measured `qb::duration`.
 */
class ScopedTimer {
public:
    using TimerCallback = std::function<void(duration)>;

    explicit ScopedTimer(TimerCallback callback)
        : _start(mono_now())
        , _callback(std::move(callback))
        , _active(true) {}

    ~ScopedTimer() {
        stop();
    }

    duration
    stop() {
        if (!_active)
            return _elapsed;
        _active  = false;
        _elapsed = mono_now() - _start;
        if (_callback)
            _callback(_elapsed);
        return _elapsed;
    }

    void
    restart() {
        _start  = mono_now();
        _active = true;
    }

    [[nodiscard]] duration
    elapsed() const {
        return _active ? (mono_now() - _start) : _elapsed;
    }

    ScopedTimer(const ScopedTimer &)            = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;
    ScopedTimer(ScopedTimer &&)                 = delete;
    ScopedTimer &operator=(ScopedTimer &&)      = delete;

private:
    mono_time     _start;
    TimerCallback _callback;
    duration      _elapsed{};
    bool          _active;
};

/**
 * @class LogTimer
 * @brief Logs the elapsed time (microseconds) of a scope on destruction.
 */
class LogTimer {
public:
    explicit LogTimer(std::string reason)
        : _reason(std::move(reason))
        , _timer([this](duration d) {
            std::fprintf(stdout, "%s: %lldus\n", _reason.c_str(),
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
        }) {}

    [[nodiscard]] duration
    elapsed() const {
        return _timer.elapsed();
    }

private:
    // Declaration order matters: `_timer`'s callback captures `this` and reads
    // `_reason` when it fires on destruction. Members are destroyed in reverse
    // declaration order, so `_timer` (declared last) is destroyed first — while
    // `_reason` is still alive. Do NOT reorder these two (would be a UAF).
    std::string _reason;
    ScopedTimer _timer;
};

// ---------------------------------------------------------------------------
// External boundary seams (the ONLY place a raw double touches time)
// ---------------------------------------------------------------------------

namespace detail {

/// Convert a span to libev's `ev_tstamp` (double seconds). Used only by the
/// async/listener/timer glue around libev.
[[nodiscard]] inline double
to_ev_seconds(duration d) noexcept {
    return std::chrono::duration_cast<std::chrono::duration<double>>(d).count();
}

/// Convert libev's `ev_tstamp` (double seconds) back to a span.
[[nodiscard]] inline duration
from_ev_seconds(double seconds) noexcept {
    return std::chrono::duration_cast<duration>(std::chrono::duration<double>(seconds));
}

} // namespace detail

} // namespace qb

#endif // QB_SYSTEM_TIME_H
