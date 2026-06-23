/**
 * @file test-timestamp.cpp
 * @brief Unit tests for the qb canonical time vocabulary (std::chrono based).
 *
 * Exercises qb::duration / qb::mono_time / qb::wall_time, the unix-epoch
 * helpers, UTC formatting/parsing, the scoped timers, the TSC counter and the
 * libev boundary seam.
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
 */

#include <gtest/gtest.h>
#include <qb/system/time.h>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// qb::duration (== std::chrono::nanoseconds)
// ---------------------------------------------------------------------------

TEST(Duration, DefaultAndExplicit) {
    qb::duration d{};
    EXPECT_EQ(d.count(), 0);

    qb::duration d1 = std::chrono::nanoseconds(1'000'000'000); // 1s
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(d1).count(), 1);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(d1).count(), 1000);
    EXPECT_EQ(d1.count(), 1'000'000'000);
}

TEST(Duration, ImplicitFromCoarserLiterals) {
    // A coarser chrono literal converts to ns implicitly and losslessly.
    qb::duration d = 30s;
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(d).count(), 30);
    d = 100ms;
    EXPECT_EQ(d.count(), 100'000'000);
    d = 5us;
    EXPECT_EQ(d.count(), 5'000);
    d = 7ns;
    EXPECT_EQ(d.count(), 7);
}

TEST(Duration, Arithmetic) {
    qb::duration a = 1s, b = 500ms;
    EXPECT_EQ((a + b).count(), 1'500'000'000);
    EXPECT_EQ((a - b).count(), 500'000'000);
    EXPECT_EQ((b * 2).count(), 1'000'000'000);
    EXPECT_EQ((3 * b).count(), 1'500'000'000);
    EXPECT_EQ((a / 2).count(), 500'000'000);
    EXPECT_EQ((-a).count(), -1'000'000'000);

    qb::duration c = 1s;
    c += 500ms;
    EXPECT_EQ(c.count(), 1'500'000'000);
    c -= 500ms;
    EXPECT_EQ(c.count(), 1'000'000'000);
}

TEST(Duration, Comparison) {
    qb::duration a = 1s, b = 500ms, c = 1s;
    EXPECT_EQ(a, c);
    EXPECT_NE(a, b);
    EXPECT_LT(b, a);
    EXPECT_GT(a, b);
    EXPECT_LE(a, c);
    EXPECT_GE(a, c);
}

TEST(Duration, ChronoInterop) {
    qb::duration d = 2min;
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::minutes>(d).count(), 2);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(d).count(), 120);

    std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(d);
    EXPECT_EQ(ms.count(), 120'000);
}

// ---------------------------------------------------------------------------
// Instants: qb::mono_time / qb::wall_time
// ---------------------------------------------------------------------------

TEST(MonoTime, NowAdvancesMonotonically) {
    auto t0 = qb::mono_now();
    std::this_thread::sleep_for(10ms);
    auto t1 = qb::mono_now();
    EXPECT_GT(t1, t0);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), 9);
}

TEST(WallTime, NowAndUnixHelpers) {
    auto w = qb::wall_now();
    EXPECT_GT(qb::unix_seconds(w), 1'600'000'000); // strictly after 2020-09
    EXPECT_EQ(qb::unix_millis(w) / 1000, qb::unix_seconds(w));
    EXPECT_EQ(qb::unix_micros(w) / 1'000'000, qb::unix_seconds(w));
    EXPECT_EQ(qb::unix_nanos(w) / 1'000'000'000, qb::unix_seconds(w));
}

TEST(WallTime, ArithmeticWithDuration) {
    // Note: wall_time's own resolution is implementation-defined (microseconds on
    // libc++), so adding a coarser-or-equal duration keeps it a wall_time, and the
    // difference of two wall_times is measured in that resolution. Assert in ms to
    // stay precision-independent.
    auto          base  = qb::wall_from_unix_seconds(1000);
    qb::wall_time later = base + std::chrono::milliseconds(500); // +0.5s
    EXPECT_EQ(qb::unix_seconds(later), 1000);
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::milliseconds>(later - base).count(), 500);

    auto t = qb::wall_from_unix_seconds(60);
    EXPECT_EQ(qb::unix_seconds(t), 60);
}

TEST(WallTime, RoundTripUnixSeconds) {
    // 2023-01-15T12:30:45Z
    auto w = qb::wall_from_unix_seconds(1'673'785'845);
    EXPECT_EQ(qb::unix_seconds(w), 1'673'785'845);
}

// ---------------------------------------------------------------------------
// UTC formatting / parsing
// ---------------------------------------------------------------------------

TEST(Format, Iso8601AndCustom) {
    auto w = qb::wall_from_unix_seconds(1'673'785'845); // 2023-01-15T12:30:45Z
    EXPECT_EQ(qb::to_iso8601(w), "2023-01-15T12:30:45Z");
    EXPECT_EQ(qb::format_utc(w, "%Y-%m-%d"), "2023-01-15");
}

TEST(Parse, Iso8601AndCustom) {
    auto a = qb::from_iso8601("2023-01-15T12:30:45Z");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(qb::unix_seconds(*a), 1'673'785'845);

    auto b = qb::parse_utc("2023/01/15 12:30:45", "%Y/%m/%d %H:%M:%S");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(qb::unix_seconds(*b), 1'673'785'845);

    EXPECT_FALSE(qb::from_iso8601("invalid-date").has_value());
    EXPECT_FALSE(qb::parse_utc("invalid-date", "%Y-%m-%d").has_value());
}

TEST(Parse, Iso8601UtcRoundTrip) {
    // Regression: from_iso8601() must parse the trailing 'Z' as UTC (timegm),
    // not local time (mktime) — otherwise the value is off by the host timezone
    // offset on any non-UTC machine and this round-trip would print a shifted
    // hour. to_iso8601() emits UTC.
    auto tp = qb::from_iso8601("2023-01-15T12:30:45Z");
    ASSERT_TRUE(tp.has_value());
    EXPECT_EQ(qb::to_iso8601(*tp), "2023-01-15T12:30:45Z");
}

// ---------------------------------------------------------------------------
// Portable UTC calendar conversions (safe_gmtime / safe_timegm / civil math).
// Pure integer, thread-safe, valid for all time_t including negative (pre-1970)
// — the cases the Windows CRT gmtime_s / _mkgmtime reject.
// ---------------------------------------------------------------------------

TEST(Calendar, DaysFromCivilKnownAnchors) {
    using qb::detail::days_from_civil;
    EXPECT_EQ(days_from_civil(1970, 1, 1), 0);       // Unix epoch
    EXPECT_EQ(days_from_civil(1969, 12, 31), -1);    // day before the epoch
    EXPECT_EQ(days_from_civil(2000, 1, 1), 10957);   // PostgreSQL epoch offset
    EXPECT_EQ(days_from_civil(1900, 1, 1), -25567);  // 70y before, with the 1900 non-leap
}

TEST(Calendar, CivilFromDaysRoundTrip) {
    // Round-trip every day across ~600 years straddling the epoch.
    for (std::int64_t day = -100000; day <= 120000; day += 7) {
        const auto c = qb::detail::civil_from_days(day);
        EXPECT_EQ(qb::detail::days_from_civil(c.year, c.month, c.day), day) << "day=" << day;
    }
    const auto epoch = qb::detail::civil_from_days(0);
    EXPECT_EQ(epoch.year, 1970);
    EXPECT_EQ(epoch.month, 1u);
    EXPECT_EQ(epoch.day, 1u);
    const auto before = qb::detail::civil_from_days(-1);
    EXPECT_EQ(before.year, 1969);
    EXPECT_EQ(before.month, 12u);
    EXPECT_EQ(before.day, 31u);
}

TEST(Calendar, SafeGmtimeKnownFields) {
    std::tm tm{};
    ASSERT_TRUE(qb::safe_gmtime(0, tm)); // 1970-01-01T00:00:00Z, a Thursday
    EXPECT_EQ(tm.tm_year, 70);
    EXPECT_EQ(tm.tm_mon, 0);
    EXPECT_EQ(tm.tm_mday, 1);
    EXPECT_EQ(tm.tm_hour, 0);
    EXPECT_EQ(tm.tm_wday, 4); // Thursday
    EXPECT_EQ(tm.tm_yday, 0);

    ASSERT_TRUE(qb::safe_gmtime(1'673'785'845, tm)); // 2023-01-15T12:30:45Z, a Sunday
    EXPECT_EQ(tm.tm_year, 123);
    EXPECT_EQ(tm.tm_mon, 0);
    EXPECT_EQ(tm.tm_mday, 15);
    EXPECT_EQ(tm.tm_hour, 12);
    EXPECT_EQ(tm.tm_min, 30);
    EXPECT_EQ(tm.tm_sec, 45);
    EXPECT_EQ(tm.tm_wday, 0);  // Sunday
    EXPECT_EQ(tm.tm_yday, 14); // 0-indexed from Jan 1
}

TEST(Calendar, SafeGmtimePreEpochFields) {
    std::tm tm{};
    ASSERT_TRUE(qb::safe_gmtime(-1, tm)); // 1969-12-31T23:59:59Z, a Wednesday
    EXPECT_EQ(tm.tm_year, 69);
    EXPECT_EQ(tm.tm_mon, 11);
    EXPECT_EQ(tm.tm_mday, 31);
    EXPECT_EQ(tm.tm_hour, 23);
    EXPECT_EQ(tm.tm_min, 59);
    EXPECT_EQ(tm.tm_sec, 59);
    EXPECT_EQ(tm.tm_wday, 3);   // Wednesday
    EXPECT_EQ(tm.tm_yday, 364); // 1969 is not a leap year
}

TEST(Calendar, SafeGmtimeTimegmRoundTrip) {
    // The whole point: exact round-trip across negative (pre-1970), zero, modern
    // and far-future instants — identical on every platform.
    const std::int64_t samples[] = {
        -62135596800LL,  // 0001-01-01T00:00:00Z
        -2208988800LL,   // 1900-01-01T00:00:00Z
        -14182940LL,     // 1969-07-20T20:17:40Z (Apollo 11)
        -1LL,            // 1969-12-31T23:59:59Z
        0LL,             // 1970-01-01T00:00:00Z
        1LL, 1'673'785'845LL, 4'102'444'800LL, // 2100-01-01T00:00:00Z
        253'402'300'799LL,                     // 9999-12-31T23:59:59Z
    };
    for (std::int64_t s : samples) {
        std::tm tm{};
        ASSERT_TRUE(qb::safe_gmtime(static_cast<std::time_t>(s), tm)) << "s=" << s;
        EXPECT_EQ(static_cast<std::int64_t>(qb::safe_timegm(tm)), s) << "s=" << s;
    }
}

TEST(Calendar, LeapYearBoundaries) {
    // 2000 is a leap year (divisible by 400), 1900 is not (divisible by 100).
    EXPECT_EQ(qb::detail::days_from_civil(2000, 2, 29) + 1, qb::detail::days_from_civil(2000, 3, 1));
    EXPECT_EQ(qb::detail::days_from_civil(1900, 2, 28) + 1, qb::detail::days_from_civil(1900, 3, 1));
    EXPECT_EQ(qb::detail::days_from_civil(2024, 2, 29) + 1, qb::detail::days_from_civil(2024, 3, 1));
    std::tm tm{};
    ASSERT_TRUE(qb::safe_gmtime(qb::safe_timegm([] { std::tm t{}; t.tm_year = 100; t.tm_mon = 1; t.tm_mday = 29; return t; }()), tm));
    EXPECT_EQ(tm.tm_mon, 1); // still February
    EXPECT_EQ(tm.tm_mday, 29);
}

TEST(Calendar, SafeLocaltimeFillsFields) {
    // Local time is TZ-dependent, so only assert it succeeds and fills a sane
    // calendar (the value depends on the host zone). Thread-safe (own tm).
    std::tm tm{};
    ASSERT_TRUE(qb::safe_localtime(1'673'785'845, tm));
    EXPECT_GE(tm.tm_mday, 1);
    EXPECT_LE(tm.tm_mday, 31);
    EXPECT_EQ(tm.tm_year, 123); // 2023 regardless of zone (a midday UTC instant)
}

TEST(Format, PreUnixEpoch) {
    // Regression for the latent Windows core bug: gmtime_s/_mkgmtime reject a
    // negative time_t, which used to make format_utc/parse_utc fail before 1970.
    auto moon = qb::wall_from_unix_seconds(-14'182'940); // 1969-07-20T20:17:40Z
    EXPECT_EQ(qb::to_iso8601(moon), "1969-07-20T20:17:40Z");
    EXPECT_EQ(qb::format_utc(moon, "%Y-%m-%d"), "1969-07-20");

    auto epoch_minus_1 = qb::wall_from_unix_seconds(-1);
    EXPECT_EQ(qb::to_iso8601(epoch_minus_1), "1969-12-31T23:59:59Z");
}

TEST(Parse, PreUnixEpochRoundTrip) {
    auto tp = qb::from_iso8601("1969-07-20T20:17:40Z");
    ASSERT_TRUE(tp.has_value());
    EXPECT_EQ(qb::unix_seconds(*tp), -14'182'940);
    EXPECT_EQ(qb::to_iso8601(*tp), "1969-07-20T20:17:40Z");

    auto far_past = qb::from_iso8601("1900-01-01T00:00:00Z");
    ASSERT_TRUE(far_past.has_value());
    EXPECT_EQ(qb::unix_seconds(*far_past), -2'208'988'800);
}

// ---------------------------------------------------------------------------
// Scoped timers
// ---------------------------------------------------------------------------

TEST(ScopedTimer, MeasuresAndCallsBack) {
    bool         invoked = false;
    qb::duration measured{};
    {
        qb::ScopedTimer timer([&](qb::duration d) {
            invoked  = true;
            measured = d;
        });
        std::this_thread::sleep_for(50ms);
        EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(timer.elapsed()).count(), 45);
    } // scope end triggers the callback
    EXPECT_TRUE(invoked);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(measured).count(), 45);
}

TEST(LogTimer, Elapsed) {
    qb::LogTimer timer("unit-test");
    std::this_thread::sleep_for(10ms);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(timer.elapsed()).count(), 9);
}

// ---------------------------------------------------------------------------
// TSC counter + libev boundary seam
// ---------------------------------------------------------------------------

TEST(Tsc, NonDecreasing) {
    auto a = qb::tsc_ticks();
    auto b = qb::tsc_ticks();
    EXPECT_GE(b, a);
}

TEST(EvSeam, RoundTrip) {
    qb::duration d = 1500ms;
    double       s = qb::detail::to_ev_seconds(d);
    EXPECT_NEAR(s, 1.5, 1e-9);
    auto d2 = qb::detail::from_ev_seconds(s);
    EXPECT_EQ(d2.count(), 1'500'000'000);
}

// ---------------------------------------------------------------------------
// Civil calendar / time-of-day value types
// ---------------------------------------------------------------------------

TEST(Date, ConstructionAndRoundTrip) {
    auto d = qb::date::from_ymd(2024, 3, 15);
    EXPECT_EQ(d.to_string(), "2024-03-15");
    EXPECT_EQ(d.days_since_epoch(), 19797);
    EXPECT_EQ(qb::date::parse("2024-03-15").value(), d);
    const auto ymd = d.year_month_day();
    EXPECT_EQ(int(ymd.year()), 2024);
    EXPECT_EQ(unsigned(ymd.month()), 3u);
    EXPECT_EQ(unsigned(ymd.day()), 15u);
    EXPECT_FALSE(qb::date::parse("not-a-date").has_value());
}

TEST(Date, PreEpochAndArithmetic) {
    auto d1900 = qb::date::from_ymd(1900, 1, 1);
    EXPECT_EQ(d1900.to_string(), "1900-01-01");
    EXPECT_EQ(d1900.days_since_epoch(), -25567);
    auto d = qb::date::from_ymd(2024, 3, 15);
    EXPECT_EQ((d + std::chrono::days{1}).to_string(), "2024-03-16");
    EXPECT_EQ((qb::date::from_ymd(2024, 3, 16) - d), std::chrono::days{1});
    EXPECT_LT(d1900, d);
    // Midnight UTC wall instant of the date.
    EXPECT_EQ(qb::unix_seconds(d.to_wall_time()), 19797LL * 86400);
    // from_wall_time floors (a pre-epoch sub-day instant stays on its calendar day).
    EXPECT_EQ(qb::date::from_wall_time(qb::wall_from_unix_seconds(-1)).to_string(), "1969-12-31");
}

TEST(TimeOfDay, ConstructionRoundTripAndHms) {
    auto t = qb::time_of_day::from_hms(14, 30, 45, 123456);
    EXPECT_EQ(t.to_string(), "14:30:45.123456");
    EXPECT_EQ(t.since_midnight().count(), 52245123456LL);
    EXPECT_EQ(qb::time_of_day::parse("14:30:45.123456").value(), t);
    const auto h = t.hms();
    EXPECT_EQ(h.hours().count(), 14);
    EXPECT_EQ(h.minutes().count(), 30);
    EXPECT_EQ(h.seconds().count(), 45);
    EXPECT_EQ(qb::time_of_day::from_hms(0, 0, 0).to_string(), "00:00:00");
    EXPECT_LT(qb::time_of_day::from_hms(8, 0, 0), t);
}

TEST(TimeOfDayTz, EastPositiveOffsetRendering) {
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, 7200).to_string(), "14:30:45+02:00");
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -18000).to_string(), "08:00:00-05:00");
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(0, 0, 0, 0, 0).to_string(), "00:00:00+00:00");
}

TEST(UtcOffset, ParseRoundTripAndForms) {
    // Inverse of format_utc_offset across the forms PostgreSQL emits.
    EXPECT_EQ(qb::parse_utc_offset("+02:00").value(), 7200);
    EXPECT_EQ(qb::parse_utc_offset("-05:00").value(), -18000);
    EXPECT_EQ(qb::parse_utc_offset("+05:30").value(), 19800); // half-hour zone
    EXPECT_EQ(qb::parse_utc_offset("-05").value(), -18000);   // hour-only form
    EXPECT_EQ(qb::parse_utc_offset("+00:00:30").value(), 30); // seconds form
    EXPECT_EQ(qb::parse_utc_offset("Z").value(), 0);
    // Round-trips with format_utc_offset for the canonical ±HH:MM form.
    for (std::int32_t s : {0, 7200, -18000, 19800, -34200})
        EXPECT_EQ(qb::parse_utc_offset(qb::format_utc_offset(s)).value(), s);
    // Malformed input -> nullopt, not a silent zero.
    EXPECT_FALSE(qb::parse_utc_offset("").has_value());
    EXPECT_FALSE(qb::parse_utc_offset("02:00").has_value());
}

TEST(CalendarInterval, LosslessAndExtractEpochFold) {
    using us = std::chrono::microseconds;
    // 1 mon 2 days 03:04:05 -> matches PG EXTRACT(EPOCH) = 2775845 s.
    EXPECT_EQ(qb::calendar_interval(1, 2, us{11045000000LL}).to_micros().count(), 2775845000000LL);
    EXPECT_EQ(qb::calendar_interval(12, 0, us{0}).to_micros().count(), 31557600000000LL); // 1 year, 365.25 d
    EXPECT_EQ(qb::calendar_interval(1, 0, us{0}).to_micros().count(), 2592000000000LL);   // 1 month, 30 d
    EXPECT_EQ(qb::calendar_interval(0, 1, us{0}).to_micros().count(), 86400000000LL);     // 1 day, 24 h
    // Components are kept separate (lossless): 1 month != 30 days as a value.
    EXPECT_NE(qb::calendar_interval(1, 0, us{0}), qb::calendar_interval(0, 30, us{0}));
    EXPECT_EQ(qb::calendar_interval(0, 0, us{-86400000000LL}).to_string(), "-24:00:00");
    EXPECT_EQ(qb::calendar_interval(1, 2, us{11045000000LL}).to_string(), "1 mon 2 days 03:04:05");
}

} // namespace
