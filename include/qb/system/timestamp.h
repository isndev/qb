/**
 * @file qb/system/timestamp.h
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
 * `std::get_time` + `timegm`, all in UTC with no tzdb dependency.
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
 * @ingroup Time
 */

#ifndef QB_SYSTEM_TIMESTAMP_H
#define QB_SYSTEM_TIMESTAMP_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iomanip>
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
    return wall_time{std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::milliseconds{ms})};
}

// ---------------------------------------------------------------------------
// UTC formatting / parsing (no time-zone database dependency)
// ---------------------------------------------------------------------------

/// Format a wall instant as UTC using a strftime-compatible format string.
/// Returns an empty string on failure.
[[nodiscard]] inline std::string
format_utc(wall_time tp, std::string_view fmt) {
    const auto      t = static_cast<std::time_t>(unix_seconds(tp));
    std::tm         tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &t) != 0)
        return {};
#else
    if (gmtime_r(&t, &tm) == nullptr)
        return {};
#endif
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
#if defined(_WIN32)
        const std::time_t t = _mkgmtime(&tm);
#else
        const std::time_t t = timegm(&tm);
#endif
        if (t == static_cast<std::time_t>(-1))
            return std::nullopt;
        return wall_from_unix_seconds(static_cast<std::int64_t>(t));
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
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count());
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
                         static_cast<long long>(
                             std::chrono::duration_cast<std::chrono::microseconds>(d).count()));
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

#endif // QB_SYSTEM_TIMESTAMP_H
