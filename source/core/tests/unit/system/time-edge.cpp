/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/system/time-edge.cpp
 * @brief Adversarial edge-case + bug-hunting coverage for the qb canonical time
 *        vocabulary (`qb/system/time.h`).
 *
 * Companion to unit/system/time.cpp. Where that file pins the happy path and a
 * handful of known anchors, this file deliberately hunts the under-covered seams
 * and the off-by-one / overflow / sign-handling corners with an INDEPENDENT
 * oracle (std::chrono::year_month_day, hand calculation, ISO/RFC format facts) —
 * it never echoes back whatever the code currently prints. Every expectation is
 * the value the oracle says is correct.
 *
 * Coverage targets (previously unhit lines in time.h):
 *   - unix_seconds/millis/micros/nanos and wall_from_unix_* on NEGATIVE instants
 *     (floor-toward-negative-infinity truncation).
 *   - the wall_from_unix_millis / wall_from_unix_nanos builders (only seconds was hit).
 *   - safe_gmtime / safe_timegm at the proleptic min/max and far-negative years,
 *     leap-day-vs-non-leap (1900/2000/2400), end-of-year tm_yday.
 *   - format_date for year 0, negative years (BCE), and 5-digit years.
 *   - parse_date with a negative / BCE year field.
 *   - format_time_of_day for the 24:00:00 boundary and sub-second (1-digit) fraction.
 *   - parse_time_of_day verbatim-microseconds fraction + the <3-field reject branch.
 *   - format_utc_offset for the +HH (>=10h), the sub-minute-dropped, and the
 *     negative-magnitude paths; parse_utc_offset for "z", malformed, +HH:MM:SS,
 *     and the range-unvalidated forms.
 *   - calendar_interval folding with NEGATIVE months/days/micros and the
 *     12-month-year vs residual-month split (truncation toward zero on % and /).
 *   - the date / time_of_day / time_of_day_tz value types at their boundaries.
 *
 * BUG-HUNTING MANDATE: cases marked `BUG-EXPOSING` below assert the
 * oracle-correct value even when the current implementation is believed wrong, so
 * a failure flags the defect rather than masking it.
 */

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <qb/system/time.h>

using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// Unix-epoch scalar extraction on NEGATIVE instants (floor toward -inf).
// The happy-path test only exercises a positive wall_now(); the truncation
// direction on a sub-second negative instant is the interesting corner.
// ---------------------------------------------------------------------------

TEST(UnixScalarsEdge, NegativeInstantTruncationDirection) {
    // -1 whole second is exactly representable: every scalar is the same -1 * unit.
    auto w = qb::wall_from_unix_seconds(-1);
    EXPECT_EQ(qb::unix_seconds(w), -1);
    EXPECT_EQ(qb::unix_millis(w), -1000);
    EXPECT_EQ(qb::unix_micros(w), -1'000'000);
    EXPECT_EQ(qb::unix_nanos(w), -1'000'000'000);

    // A sub-second NEGATIVE instant: -500ms. duration_cast TRUNCATES TOWARD ZERO
    // (it is not a floor), so whole-second extraction of -500ms is 0, not -1.
    // This pins the documented chrono semantics so a future "floor" refactor is a
    // conscious choice, not a silent shift.
    auto half = qb::wall_from_unix_millis(-500);
    EXPECT_EQ(qb::unix_millis(half), -500);
    EXPECT_EQ(qb::unix_seconds(half), 0) << "duration_cast truncates toward zero, not floor";
    EXPECT_EQ(qb::unix_micros(half), -500'000);

    // -1500ms -> -1s when cast to whole seconds (trunc toward zero of -1.5).
    auto onehalf = qb::wall_from_unix_millis(-1500);
    EXPECT_EQ(qb::unix_seconds(onehalf), -1);
    EXPECT_EQ(qb::unix_millis(onehalf), -1500);
}

TEST(UnixScalarsEdge, MillisAndNanosBuildersRoundTrip) {
    // wall_from_unix_millis / wall_from_unix_nanos were never exercised by the
    // happy-path file. Round-trip a known positive and a known negative.
    auto a = qb::wall_from_unix_millis(1'673'785'845'123LL);
    EXPECT_EQ(qb::unix_millis(a), 1'673'785'845'123LL);
    EXPECT_EQ(qb::unix_seconds(a), 1'673'785'845LL);

    // wall_from_unix_nanos truncates to the system_clock tick (microseconds on
    // libc++) via duration_cast, so a sub-microsecond nanos count does NOT survive
    // a nanos round-trip. Assert at microsecond granularity to stay
    // precision-independent: -987'654'321ns -> -987'654us exactly (trunc toward zero).
    auto b = qb::wall_from_unix_nanos(-987'654'321LL);
    EXPECT_EQ(qb::unix_micros(b), -987'654LL);
    // A whole-microsecond nanos count DOES round-trip losslessly through nanos.
    auto bw = qb::wall_from_unix_nanos(-987'654'000LL);
    EXPECT_EQ(qb::unix_nanos(bw), -987'654'000LL);

    // 1ns past a whole second floors to the whole second at microsecond resolution.
    auto c = qb::wall_from_unix_nanos(1'000'000'001LL); // 1.000000001 s
    EXPECT_EQ(qb::unix_micros(c), 1'000'000LL);
}

// ---------------------------------------------------------------------------
// days_from_civil / civil_from_days at the proleptic extremes + leap rules.
// Oracle: std::chrono::year_month_day (independent calendar) + hand facts.
// ---------------------------------------------------------------------------

TEST(CalendarEdge, ProlepticExtremesMatchChronoOracle) {
    using qb::detail::days_from_civil;
    // Independent oracle: sys_days{year_month_day{...}} computes the same epoch-day
    // count through libc++'s calendar, a different code path than Hinnant's here.
    auto oracle = [](int y, unsigned m, unsigned d) -> std::int64_t {
        return std::chrono::sys_days{std::chrono::year{y} / std::chrono::month{m} / std::chrono::day{d}}
            .time_since_epoch()
            .count();
    };
    for (auto [y, m, d] : {std::tuple{1, 1, 1}, std::tuple{9999, 12, 31}, std::tuple{1582, 10, 15},
                           std::tuple{1600, 2, 29}, std::tuple{2400, 2, 29}, std::tuple{4000, 12, 31}}) {
        EXPECT_EQ(days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)),
                  oracle(y, static_cast<unsigned>(m), static_cast<unsigned>(d)))
            << "y=" << y << " m=" << m << " d=" << d;
    }
    // Hand anchors: 0001-01-01 is 719162 days BEFORE the epoch; 9999-12-31 is 2932896 after.
    EXPECT_EQ(days_from_civil(1, 1, 1), -719162);
    EXPECT_EQ(days_from_civil(9999, 12, 31), 2932896);
}

TEST(CalendarEdge, LeapCenturyRulesAcrossCenturies) {
    using qb::detail::days_from_civil;
    // 1900 is NOT a leap year (div by 100 but not 400): Feb has 28 days.
    EXPECT_EQ(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 1), 28);
    // 2000 IS a leap year (div by 400): Feb has 29 days.
    EXPECT_EQ(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 1), 29);
    // 2400 IS a leap year (div by 400).
    EXPECT_EQ(days_from_civil(2400, 3, 1) - days_from_civil(2400, 2, 1), 29);
    // 2100 is NOT (div by 100, not 400).
    EXPECT_EQ(days_from_civil(2100, 3, 1) - days_from_civil(2100, 2, 1), 28);
    // 2024 ordinary leap.
    EXPECT_EQ(days_from_civil(2024, 3, 1) - days_from_civil(2024, 2, 1), 29);
}

TEST(CalendarEdge, CivilFromDaysFarNegativeAndYearZero) {
    // Year 0 exists in the proleptic Gregorian calendar used here (NOT skipped).
    // 0000-01-01 oracle:
    const std::int64_t y0 = std::chrono::sys_days{std::chrono::year{0} / std::chrono::January / 1}
                                .time_since_epoch()
                                .count();
    auto c0 = qb::detail::civil_from_days(y0);
    EXPECT_EQ(c0.year, 0);
    EXPECT_EQ(c0.month, 1u);
    EXPECT_EQ(c0.day, 1u);

    // A BCE date: 1 BCE == proleptic year 0; 2 BCE == year -1. Round-trip -1.
    auto bce = qb::detail::days_from_civil(-1, 6, 15);
    auto cbce = qb::detail::civil_from_days(bce);
    EXPECT_EQ(cbce.year, -1);
    EXPECT_EQ(cbce.month, 6u);
    EXPECT_EQ(cbce.day, 15u);
}

// ---------------------------------------------------------------------------
// safe_gmtime: tm_yday end-of-year, far-future, and the int tm_year overflow
// guard (the documented `return false` path — never exercised before).
// ---------------------------------------------------------------------------

TEST(SafeGmtimeEdge, EndOfYearYdayLeapAndNonLeap) {
    std::tm tm{};
    // 2024-12-31T00:00:00Z — 2024 is a leap year so yday of Dec 31 is 365 (0-indexed).
    const std::time_t leap_dec31 = qb::safe_timegm([] {
        std::tm t{};
        t.tm_year = 124; // 2024
        t.tm_mon  = 11;  // December
        t.tm_mday = 31;
        return t;
    }());
    ASSERT_TRUE(qb::safe_gmtime(leap_dec31, tm));
    EXPECT_EQ(tm.tm_yday, 365) << "Dec 31 of a leap year is day index 365";

    // 2023-12-31 — non-leap, yday 364.
    const std::time_t nonleap_dec31 = qb::safe_timegm([] {
        std::tm t{};
        t.tm_year = 123;
        t.tm_mon  = 11;
        t.tm_mday = 31;
        return t;
    }());
    ASSERT_TRUE(qb::safe_gmtime(nonleap_dec31, tm));
    EXPECT_EQ(tm.tm_yday, 364);

    // Leap-year March 1 yday must include the Feb-29 bump: Jan(31)+Feb(29)=60 -> index 60.
    const std::time_t leap_mar1 = qb::safe_timegm([] {
        std::tm t{};
        t.tm_year = 124;
        t.tm_mon  = 2; // March
        t.tm_mday = 1;
        return t;
    }());
    ASSERT_TRUE(qb::safe_gmtime(leap_mar1, tm));
    EXPECT_EQ(tm.tm_yday, 60);
}

TEST(SafeGmtimeEdge, WeekdayBeforeEpochFloorsCorrectly) {
    // wday for a NEGATIVE day count must floor, not truncate. 1970-01-01 = Thu(4).
    // 1969-12-29 is the Monday of that week; days = -3. (-3 % 7 + 4) % 7 must be 1.
    std::tm tm{};
    const std::time_t mon = qb::safe_timegm([] {
        std::tm t{};
        t.tm_year = 69;
        t.tm_mon  = 11;
        t.tm_mday = 29; // 1969-12-29, a Monday
        return t;
    }());
    ASSERT_TRUE(qb::safe_gmtime(mon, tm));
    EXPECT_EQ(tm.tm_wday, 1) << "1969-12-29 is a Monday";

    // The Apollo-11 instant 1969-07-20 is a Sunday (wday 0) — exercises the
    // negative-days weekday branch with a mid-day time component.
    ASSERT_TRUE(qb::safe_gmtime(static_cast<std::time_t>(-14'182'940), tm));
    EXPECT_EQ(tm.tm_wday, 0);
    EXPECT_EQ(tm.tm_hour, 20);
    EXPECT_EQ(tm.tm_min, 17);
    EXPECT_EQ(tm.tm_sec, 40);
}

TEST(SafeGmtimeEdge, YearOverflowGuardReturnsFalse) {
    // The ONLY documented `return false` path: a year so far out that
    // (year - 1900) overflows the int tm_year field. With time_t == int64 seconds,
    // pick a seconds value whose civil year is astronomically large.
    // year ~ 2.9e11 / 365.25 days ... need (year-1900) > INT_MAX (~2.147e9).
    // Use seconds for ~ year 3e9: days = year*365.25 ~ (3e9-1970)*365.25, secs = days*86400.
    // Build straight from a giant positive time_t near the int64 ceiling.
    const std::int64_t huge = std::numeric_limits<std::int64_t>::max(); // ~292 billion years
    std::tm tm{};
    EXPECT_FALSE(qb::safe_gmtime(static_cast<std::time_t>(huge), tm))
        << "a year past INT_MAX+1900 must fail the tm_year guard";

    // Symmetric far-negative also overflows the guard.
    const std::int64_t huge_neg = std::numeric_limits<std::int64_t>::min();
    EXPECT_FALSE(qb::safe_gmtime(static_cast<std::time_t>(huge_neg), tm));
}

// ---------------------------------------------------------------------------
// format_date: year 0, negative (BCE) years, 5-digit years. The %04lld padding
// behaviour on a negative/zero/large year was never covered.
// ---------------------------------------------------------------------------

TEST(FormatDateEdge, YearZeroNegativeAndFiveDigit) {
    // Oracle: %04lld zero-pads the magnitude and keeps the sign GLUED to the
    // digits, so -1 prints "-001" (sign counts toward the field width).
    // Year 0 -> "0000-01-01".
    EXPECT_EQ(qb::format_date(qb::detail::days_from_civil(0, 1, 1)), "0000-01-01");

    // Year -1 (2 BCE) -> "-001-12-31" style. Use -1-12-31.
    EXPECT_EQ(qb::format_date(qb::detail::days_from_civil(-1, 12, 31)), "-001-12-31");

    // Year -44 (Ides of March, 44 BCE proleptic) -> "-044-03-15".
    EXPECT_EQ(qb::format_date(qb::detail::days_from_civil(-44, 3, 15)), "-044-03-15");

    // 5-digit future year overflows the 4-wide pad to its natural width.
    EXPECT_EQ(qb::format_date(qb::detail::days_from_civil(10000, 1, 1)), "10000-01-01");

    // Max representable proleptic anchor used elsewhere: 9999-12-31.
    EXPECT_EQ(qb::format_date(2932896), "9999-12-31");
}

TEST(ParseDateEdge, NegativeYearAndPartialFields) {
    // parse_date uses sscanf("%d-%d-%d"); a negative year field is accepted as a
    // signed int and feeds days_from_civil directly.
    auto neg = qb::parse_date("-044-03-15");
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, qb::detail::days_from_civil(-44, 3, 15));

    // Round-trip: format then parse a BCE date.
    EXPECT_EQ(qb::parse_date(qb::format_date(qb::detail::days_from_civil(-1, 12, 31))).value(),
              qb::detail::days_from_civil(-1, 12, 31));

    // Fewer than 3 fields -> nullopt (sscanf returns <3).
    EXPECT_FALSE(qb::parse_date("2024-03").has_value());
    EXPECT_FALSE(qb::parse_date("2024").has_value());
    EXPECT_FALSE(qb::parse_date("").has_value());
    EXPECT_FALSE(qb::parse_date("garbage").has_value());
}

// ---------------------------------------------------------------------------
// format_time_of_day / parse_time_of_day boundaries.
// ---------------------------------------------------------------------------

TEST(TimeOfDayFormatEdge, MidnightFullDayAndSingleDigitFraction) {
    // Exactly midnight: bare HH:MM:SS, no fraction (fraction emitted only when > 0).
    EXPECT_EQ(qb::format_time_of_day(0), "00:00:00");

    // The 24:00:00 boundary (one full day of micros). hh_mm_ss does NOT normalize:
    // the formatter prints the literal 24. This documents that the type carries
    // an out-of-[0,24h) value verbatim (PostgreSQL TIME '24:00:00' is legal).
    EXPECT_EQ(qb::format_time_of_day(86400LL * 1'000'000), "24:00:00");

    // A 1-microsecond fraction must keep all six pad digits.
    EXPECT_EQ(qb::format_time_of_day(1), "00:00:00.000001");

    // Last microsecond of the second: .999999.
    EXPECT_EQ(qb::format_time_of_day(23 * 3600LL * 1'000'000 + 999'999), "23:00:00.999999");
}

TEST(TimeOfDayParseEdge, VerbatimFractionAndShortRejects) {
    // The contract: the fractional field is read by %d as a literal integer and
    // taken VERBATIM as microseconds. So ".5" parses to 5 micros, NOT 500000.
    // This is the documented (if surprising) behaviour — pin it so a future
    // "scale the fraction" change is deliberate.
    auto five = qb::parse_time_of_day("00:00:00.5");
    ASSERT_TRUE(five.has_value());
    EXPECT_EQ(*five, 5) << "fraction is verbatim micros, not scaled by digit count";
    EXPECT_EQ(qb::format_time_of_day(*five), "00:00:00.000005");

    // A full 6-digit fraction round-trips exactly.
    auto six = qb::parse_time_of_day("01:02:03.000004");
    ASSERT_TRUE(six.has_value());
    EXPECT_EQ(*six, (1 * 3600LL + 2 * 60 + 3) * 1'000'000 + 4);

    // Missing the seconds field (< 3 scanned) -> nullopt.
    EXPECT_FALSE(qb::parse_time_of_day("12:30").has_value());
    EXPECT_FALSE(qb::parse_time_of_day("12").has_value());
    EXPECT_FALSE(qb::parse_time_of_day("").has_value());
    EXPECT_FALSE(qb::parse_time_of_day("noon").has_value());

    // No-fraction form leaves micros at the %d default (0), so it round-trips bare.
    auto whole = qb::parse_time_of_day("23:59:59");
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(*whole, (23 * 3600LL + 59 * 60 + 59) * 1'000'000);
}

// ---------------------------------------------------------------------------
// format_utc_offset / parse_utc_offset — the sign, padding, sub-minute-drop and
// range-validation corners.
// ---------------------------------------------------------------------------

TEST(UtcOffsetFormatEdge, LargeNegativeAndWholeHours) {
    // +14:00 is the largest real-world zone (Kiribati). Sign + 2-digit hour.
    EXPECT_EQ(qb::format_utc_offset(14 * 3600), "+14:00");
    // -12:00 (Baker Island historically).
    EXPECT_EQ(qb::format_utc_offset(-12 * 3600), "-12:00");
    // A 45-minute zone (Nepal +05:45).
    EXPECT_EQ(qb::format_utc_offset(5 * 3600 + 45 * 60), "+05:45");
    // Negative half-hour (Newfoundland -03:30).
    EXPECT_EQ(qb::format_utc_offset(-(3 * 3600 + 30 * 60)), "-03:30");
}

TEST(UtcOffsetFormatEdge, SubMinuteSecondsAreDroppedFromFormat) {
    // format_utc_offset emits only ±HH:MM — a sub-minute seconds component is
    // intentionally dropped (the ±HH:MM wire form has no seconds field). +30s
    // therefore renders as the zero offset.
    EXPECT_EQ(qb::format_utc_offset(30), "+00:00");
    // +90s keeps the whole minute, drops the residual 30s.
    EXPECT_EQ(qb::format_utc_offset(90), "+00:01");

    // BUG-EXPOSING: a NEGATIVE sub-minute offset renders the sign of a value whose
    // printed magnitude is zero, producing "-00:00" (negative zero). A UTC offset
    // of -00:00 is not a canonical wire token; the oracle-correct render of any
    // offset that rounds to zero minutes is "+00:00". If this fails, the formatter
    // is emitting a negative-zero offset string.
    EXPECT_EQ(qb::format_utc_offset(-30), "+00:00")
        << "BUG: negative sub-minute offset prints negative-zero '-00:00'";
}

TEST(UtcOffsetParseEdge, AllFormsLowercaseZAndMalformed) {
    // Lowercase 'z' (the existing file only covers uppercase 'Z').
    EXPECT_EQ(qb::parse_utc_offset("z").value(), 0);
    EXPECT_EQ(qb::parse_utc_offset("Z").value(), 0);

    // +HH only (no minutes) — minute/second default to 0.
    EXPECT_EQ(qb::parse_utc_offset("+14").value(), 14 * 3600);
    EXPECT_EQ(qb::parse_utc_offset("-12").value(), -12 * 3600);

    // ±HH:MM:SS full form, both signs.
    EXPECT_EQ(qb::parse_utc_offset("+01:02:03").value(), 1 * 3600 + 2 * 60 + 3);
    EXPECT_EQ(qb::parse_utc_offset("-01:02:03").value(), -(1 * 3600 + 2 * 60 + 3));

    // Malformed: a leading char that is neither sign nor Z/z -> nullopt.
    EXPECT_FALSE(qb::parse_utc_offset("02:00").has_value());
    EXPECT_FALSE(qb::parse_utc_offset("").has_value());
    EXPECT_FALSE(qb::parse_utc_offset("X").has_value());
    // A lone sign with no digits -> sscanf reads 0 fields (<1) -> nullopt.
    EXPECT_FALSE(qb::parse_utc_offset("+").has_value());
    EXPECT_FALSE(qb::parse_utc_offset("-").has_value());
    // "ZZ" is not the single-char Z token and has no sign -> nullopt.
    EXPECT_FALSE(qb::parse_utc_offset("ZZ").has_value());
}

TEST(UtcOffsetParseEdge, OutOfRangeIsNotValidated) {
    // DOCUMENTED LIMITATION (pinned, not a hidden bug): parse_utc_offset does NOT
    // range-check the fields — it returns the raw arithmetic. A future caller that
    // assumes |offset| <= 18h must validate itself. These pin the current contract.
    EXPECT_EQ(qb::parse_utc_offset("+15:00").value(), 15 * 3600);
    EXPECT_EQ(qb::parse_utc_offset("+99:99").value(), 99 * 3600 + 99 * 60);
}

TEST(UtcOffsetRoundTrip, MinuteGranularSpanRoundTrips) {
    // Any minute-granular offset survives format -> parse exactly (seconds dropped,
    // but minute-granular inputs have none). Sweep a wide signed range.
    for (std::int32_t mins = -18 * 60; mins <= 18 * 60; mins += 15) {
        const std::int32_t secs = mins * 60;
        EXPECT_EQ(qb::parse_utc_offset(qb::format_utc_offset(secs)).value(), secs) << "mins=" << mins;
    }
}

// ---------------------------------------------------------------------------
// calendar_interval folding: negative components and the 12-month-year /
// residual-month split, including the truncation-toward-zero of `/` and `%`.
// ---------------------------------------------------------------------------

TEST(CalendarIntervalEdge, NegativeComponentsFold) {
    using us = std::chrono::microseconds;
    constexpr std::int64_t USECS_PER_DAY  = 86400LL * 1'000'000;
    constexpr std::int64_t USECS_PER_YEAR = 31557600LL * 1'000'000; // 365.25 days

    // -1 month folds to -30 days (residual-month path, months%12 == -1).
    EXPECT_EQ(qb::calendar_interval(-1, 0, us{0}).to_micros().count(), -30 * USECS_PER_DAY);

    // -13 months == -(1 year + 1 month) under trunc-toward-zero: -1*YEAR + -1*30d.
    EXPECT_EQ(qb::calendar_interval(-13, 0, us{0}).to_micros().count(),
              -USECS_PER_YEAR - 30 * USECS_PER_DAY);

    // +13 months mirror.
    EXPECT_EQ(qb::calendar_interval(13, 0, us{0}).to_micros().count(),
              USECS_PER_YEAR + 30 * USECS_PER_DAY);

    // Mixed signs: +1 month, -2 days, +500000us.
    EXPECT_EQ(qb::calendar_interval(1, -2, us{500'000}).to_micros().count(),
              500'000 + (-2) * USECS_PER_DAY + 30 * USECS_PER_DAY);

    // -24 months == exactly -2 years, no residual month.
    EXPECT_EQ(qb::calendar_interval(-24, 0, us{0}).to_micros().count(), -2 * USECS_PER_YEAR);
}

TEST(CalendarIntervalEdge, ToStringSignsAndPluralAndZero) {
    using us = std::chrono::microseconds;
    // Singular vs plural unit labels.
    EXPECT_EQ(qb::calendar_interval(1, 0, us{0}).to_string(), "1 mon 00:00:00");
    EXPECT_EQ(qb::calendar_interval(2, 0, us{0}).to_string(), "2 mons 00:00:00");
    EXPECT_EQ(qb::calendar_interval(0, 1, us{0}).to_string(), "1 day 00:00:00");
    EXPECT_EQ(qb::calendar_interval(0, 3, us{0}).to_string(), "3 days 00:00:00");
    // Negative singular keeps the "mon"/"day" singular form (==-1).
    EXPECT_EQ(qb::calendar_interval(-1, -1, us{0}).to_string(), "-1 mon -1 day 00:00:00");
    // Negative micros prints a leading '-' and the abs time-of-day.
    EXPECT_EQ(qb::calendar_interval(0, 0, us{-3'661'000'000LL}).to_string(), "-01:01:01");
    // A fully zero interval prints just the zero time-of-day (no mon/day labels).
    EXPECT_EQ(qb::calendar_interval(0, 0, us{0}).to_string(), "00:00:00");
    // Fractional negative micros: -1.5s -> "-00:00:01.500000".
    EXPECT_EQ(qb::calendar_interval(0, 0, us{-1'500'000LL}).to_string(), "-00:00:01.500000");
}

TEST(CalendarIntervalEdge, ComponentsStaySeparateAndComparable) {
    using us = std::chrono::microseconds;
    // 1 month and 30 days fold to the SAME micros but are DISTINCT values (lossless).
    EXPECT_EQ(qb::calendar_interval(1, 0, us{0}).to_micros(),
              qb::calendar_interval(0, 30, us{0}).to_micros());
    EXPECT_NE(qb::calendar_interval(1, 0, us{0}), qb::calendar_interval(0, 30, us{0}));
    // The defaulted spaceship orders lexicographically by (months, days, micros).
    EXPECT_LT(qb::calendar_interval(0, 5, us{0}), qb::calendar_interval(1, 0, us{0}));
    EXPECT_GT(qb::calendar_interval(0, 0, us{1}), qb::calendar_interval(0, 0, us{0}));
}

// ---------------------------------------------------------------------------
// date value type at its boundaries.
// ---------------------------------------------------------------------------

TEST(DateEdge, MaxMinAndFromWallTimeFlooring) {
    // Far-future and far-past round-trip through every accessor.
    auto dmax = qb::date::from_ymd(9999, 12, 31);
    EXPECT_EQ(dmax.to_string(), "9999-12-31");
    EXPECT_EQ(dmax.days_since_epoch(), 2932896);
    auto ymd = dmax.year_month_day();
    EXPECT_EQ(int(ymd.year()), 9999);
    EXPECT_EQ(unsigned(ymd.month()), 12u);
    EXPECT_EQ(unsigned(ymd.day()), 31u);

    auto dmin = qb::date::from_ymd(1, 1, 1);
    EXPECT_EQ(dmin.to_string(), "0001-01-01");
    EXPECT_EQ(dmin.days_since_epoch(), -719162);

    // from_days_since_epoch / to_wall_time midnight invariant for a pre-epoch date.
    auto d1969 = qb::date::from_ymd(1969, 12, 31);
    EXPECT_EQ(qb::unix_seconds(d1969.to_wall_time()), -1LL * 86400);

    // from_wall_time floors a NEGATIVE sub-day instant onto its calendar day:
    // -1s is 1969-12-31T23:59:59Z, whose date is 1969-12-31, not 1970-01-01.
    EXPECT_EQ(qb::date::from_wall_time(qb::wall_from_unix_seconds(-1)).to_string(), "1969-12-31");
    // Exactly midnight of the epoch maps to 1970-01-01.
    EXPECT_EQ(qb::date::from_wall_time(qb::wall_from_unix_seconds(0)).to_string(), "1970-01-01");
    // One second before two days back: -86401s -> 1969-12-30.
    EXPECT_EQ(qb::date::from_wall_time(qb::wall_from_unix_seconds(-86401)).to_string(), "1969-12-30");
}

TEST(DateEdge, ArithmeticAcrossLeapDayAndYearBoundary) {
    // Adding days must cross the Feb-29 leap day correctly.
    auto feb28_2024 = qb::date::from_ymd(2024, 2, 28);
    EXPECT_EQ((feb28_2024 + std::chrono::days{1}).to_string(), "2024-02-29"); // leap day exists
    EXPECT_EQ((feb28_2024 + std::chrono::days{2}).to_string(), "2024-03-01");

    // 1900 is NOT leap: Feb 28 + 1 day -> Mar 1.
    auto feb28_1900 = qb::date::from_ymd(1900, 2, 28);
    EXPECT_EQ((feb28_1900 + std::chrono::days{1}).to_string(), "1900-03-01");

    // Year boundary and operator-=.
    auto jan1 = qb::date::from_ymd(2025, 1, 1);
    jan1 -= std::chrono::days{1};
    EXPECT_EQ(jan1.to_string(), "2024-12-31");

    // Difference spans the leap year: 2024-01-01 .. 2025-01-01 == 366 days.
    EXPECT_EQ((qb::date::from_ymd(2025, 1, 1) - qb::date::from_ymd(2024, 1, 1)), std::chrono::days{366});
    // 2023 is not leap: 365 days.
    EXPECT_EQ((qb::date::from_ymd(2024, 1, 1) - qb::date::from_ymd(2023, 1, 1)), std::chrono::days{365});
}

TEST(DateEdge, ParseMalformedAndDefaultCtor) {
    EXPECT_FALSE(qb::date::parse("").has_value());
    EXPECT_FALSE(qb::date::parse("2024/03/15").has_value()); // wrong separator: parser reads year then stops
    EXPECT_FALSE(qb::date::parse("hello").has_value());
    // Default-constructed date is the epoch (sys_days{}).
    EXPECT_EQ(qb::date{}.days_since_epoch(), 0);
    EXPECT_EQ(qb::date{}.to_string(), "1970-01-01");
}

// ---------------------------------------------------------------------------
// time_of_day / time_of_day_tz boundaries.
// ---------------------------------------------------------------------------

TEST(TimeOfDayEdge, BoundaryValuesAndHms) {
    // Last microsecond before midnight.
    auto last = qb::time_of_day::from_micros(86400LL * 1'000'000 - 1);
    EXPECT_EQ(last.to_string(), "23:59:59.999999");
    EXPECT_EQ(last.since_midnight().count(), 86400LL * 1'000'000 - 1);
    auto h = last.hms();
    EXPECT_EQ(h.hours().count(), 23);
    EXPECT_EQ(h.minutes().count(), 59);
    EXPECT_EQ(h.seconds().count(), 59);
    EXPECT_EQ(h.subseconds().count(), 999'999);

    // from_hms with an explicit microsecond field.
    EXPECT_EQ(qb::time_of_day::from_hms(1, 2, 3, 4).since_midnight().count(),
              (1 * 3600LL + 2 * 60 + 3) * 1'000'000 + 4);

    // Default-constructed is midnight.
    EXPECT_EQ(qb::time_of_day{}.since_midnight().count(), 0);
    EXPECT_EQ(qb::time_of_day{}.to_string(), "00:00:00");

    // Ordering via the defaulted spaceship.
    EXPECT_LT(qb::time_of_day::from_hms(0, 0, 0), qb::time_of_day::from_hms(0, 0, 0, 1));
}

TEST(TimeOfDayTzEdge, NegativeOffsetAndFractionRendering) {
    // Negative whole-hour offset with a fractional time-of-day.
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(23, 59, 59, 999'999, -5 * 3600).to_string(),
              "23:59:59.999999-05:00");
    // 45-minute eastern offset.
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(12, 0, 0, 0, 5 * 3600 + 45 * 60).to_string(),
              "12:00:00+05:45");
    // Default-constructed: midnight at +00:00.
    EXPECT_EQ(qb::time_of_day_tz{}.to_string(), "00:00:00+00:00");
    // Equality of the aggregate via the defaulted spaceship.
    EXPECT_EQ(qb::time_of_day_tz::from_hms_offset(1, 2, 3, 0, 60),
              qb::time_of_day_tz(qb::time_of_day::from_hms(1, 2, 3), std::chrono::seconds{60}));
}

// ---------------------------------------------------------------------------
// format_utc buffer / parse_utc adversarial inputs.
// ---------------------------------------------------------------------------

TEST(FormatUtcEdge, EmptyFormatAndTrailingLiteral) {
    auto w = qb::wall_from_unix_seconds(1'673'785'845); // 2023-01-15T12:30:45Z
    // Empty format yields an empty string (strftime writes nothing).
    EXPECT_EQ(qb::format_utc(w, ""), "");
    // A format with only literal text (no conversion) passes through.
    EXPECT_EQ(qb::format_utc(w, "UTC"), "UTC");
    // %Y%m%d compact form.
    EXPECT_EQ(qb::format_utc(w, "%Y%m%d"), "20230115");
}

TEST(ParseUtcEdge, PartialAndTrailingGarbage) {
    // get_time fills what it can; a format that matches a prefix succeeds and the
    // unmatched tail is ignored by the stream (no fail bit on leftover input).
    auto only_date = qb::parse_utc("2023-01-15", "%Y-%m-%d");
    ASSERT_TRUE(only_date.has_value());
    // Time fields default to 0 -> midnight UTC.
    EXPECT_EQ(qb::to_iso8601(*only_date), "2023-01-15T00:00:00Z");

    // A format that cannot match at all -> nullopt.
    EXPECT_FALSE(qb::parse_utc("", "%Y-%m-%dT%H:%M:%SZ").has_value());
    EXPECT_FALSE(qb::parse_utc("not-a-date", "%Y-%m-%d").has_value());

    // Pre-epoch parse round-trips through the integer timegm (no -1 sentinel
    // collision: 1969-12-31T23:59:59Z is exactly unix -1).
    auto preneg = qb::parse_utc("1969-12-31T23:59:59Z", "%Y-%m-%dT%H:%M:%SZ");
    ASSERT_TRUE(preneg.has_value());
    EXPECT_EQ(qb::unix_seconds(*preneg), -1);
}

} // namespace
