/**
 * @file qb/source/core/tests/unit/system/parse.cpp
 * @brief Unit coverage for qb::to_number / qb::to_number_prefix (qb/system/parse.h).
 *
 * Pins the contract the codebase-wide std::sto* -> std::from_chars migration
 * depends on: strict whole-string parsing, the lenient stoi/strtol prefix idiom,
 * locale-independence, base support, range-checking, and the subnormal / inf /
 * nan floating-point edges that std::stod/std::stof get wrong.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 */

#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <qb/system/parse.h>

using qb::to_number;
using qb::to_number_prefix;

// ============================================================================
// to_number — STRICT integral
// ============================================================================

TEST(QbToNumber, StrictIntBasic) {
    EXPECT_EQ(to_number<int>("0"), 0);
    EXPECT_EQ(to_number<int>("42"), 42);
    EXPECT_EQ(to_number<int>("-42"), -42);
    EXPECT_EQ(to_number<long long>("9223372036854775807"), std::numeric_limits<long long>::max());
    EXPECT_EQ(to_number<long long>("-9223372036854775808"), std::numeric_limits<long long>::min());
    EXPECT_EQ(to_number<std::uint64_t>("18446744073709551615"), std::numeric_limits<std::uint64_t>::max());
}

TEST(QbToNumber, StrictIntRejectsSurroundingsAndJunk) {
    EXPECT_FALSE(to_number<int>("").has_value());      // empty
    EXPECT_FALSE(to_number<int>(" 42").has_value());   // leading whitespace
    EXPECT_FALSE(to_number<int>("42 ").has_value());   // trailing whitespace
    EXPECT_FALSE(to_number<int>("+42").has_value());   // leading '+'
    EXPECT_FALSE(to_number<int>("12abc").has_value()); // trailing junk
    EXPECT_FALSE(to_number<int>("0x10").has_value());  // base-10: 'x' is junk
    EXPECT_FALSE(to_number<int>("abc").has_value());
}

TEST(QbToNumber, StrictIntRangeChecked) {
    // smallint-width overflow is reported, never wrapped.
    EXPECT_EQ(to_number<std::int16_t>("32767"), 32767);
    EXPECT_FALSE(to_number<std::int16_t>("32768").has_value());
    EXPECT_FALSE(to_number<std::int16_t>("-32769").has_value());
    // unsigned rejects a negative sign (no silent wrap to a huge value).
    EXPECT_FALSE(to_number<unsigned>("-1").has_value());
}

TEST(QbToNumber, StrictIntBase) {
    EXPECT_EQ(to_number<int>("ff", 16), 255);
    EXPECT_EQ(to_number<int>("FF", 16), 255);
    EXPECT_EQ(to_number<unsigned>("ab", 16), 0xABu);
    EXPECT_EQ(to_number<int>("777", 8), 0777);
    EXPECT_EQ(to_number<int>("101", 2), 5);
    // A digit outside the base is junk -> nullopt (strict consumes the whole view).
    EXPECT_FALSE(to_number<int>("0x10", 16).has_value()); // 'x' invalid in hex
    EXPECT_FALSE(to_number<int>("9", 8).has_value());
}

// ============================================================================
// to_number — STRICT floating-point (the std::stod failure modes)
// ============================================================================

TEST(QbToNumber, StrictFloatBasic) {
    EXPECT_DOUBLE_EQ(*to_number<double>("2.5"), 2.5);
    EXPECT_DOUBLE_EQ(*to_number<double>("-0.0"), 0.0);
    EXPECT_TRUE(std::signbit(*to_number<double>("-0.0")));
    EXPECT_DOUBLE_EQ(*to_number<double>("1.5e10"), 1.5e10);
    EXPECT_FLOAT_EQ(*to_number<float>("3.14"), 3.14f);
}

TEST(QbToNumber, StrictFloatSubnormalParsesExactly) {
    // std::stod / std::stof THROW std::out_of_range here; from_chars is exact.
    const double dsub = std::numeric_limits<double>::denorm_min();
    auto         d    = to_number<double>("5e-324");
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(*d, dsub);

    const float fsub = std::numeric_limits<float>::denorm_min();
    auto        f    = to_number<float>("1e-45");
    ASSERT_TRUE(f.has_value());
    EXPECT_FLOAT_EQ(*f, fsub);
}

TEST(QbToNumber, StrictFloatInfNanCaseInsensitive) {
    EXPECT_TRUE(std::isinf(*to_number<double>("inf")));
    EXPECT_TRUE(std::isinf(*to_number<double>("Infinity")));
    EXPECT_TRUE(std::isinf(*to_number<double>("INF")));
    EXPECT_GT(*to_number<double>("inf"), 0.0);
    EXPECT_LT(*to_number<double>("-inf"), 0.0);
    EXPECT_TRUE(std::isnan(*to_number<double>("nan")));
    EXPECT_TRUE(std::isnan(*to_number<double>("NAN")));
}

TEST(QbToNumber, StrictFloatOverflowRejected) {
    EXPECT_FALSE(to_number<double>("1e400").has_value()); // > DBL_MAX
    EXPECT_FALSE(to_number<float>("1e40").has_value());   // > FLT_MAX
}

TEST(QbToNumber, StrictFloatRejectsSurroundingsAndJunk) {
    EXPECT_FALSE(to_number<double>("").has_value());
    EXPECT_FALSE(to_number<double>(" 2.5").has_value());
    EXPECT_FALSE(to_number<double>("2.5 ").has_value());
    EXPECT_FALSE(to_number<double>("2.5x").has_value());
    EXPECT_FALSE(to_number<double>("+2.5").has_value()); // strict rejects '+'
    EXPECT_FALSE(to_number<double>("garbage").has_value());
}

// ============================================================================
// to_number_prefix — the std::stoi / std::strtol idiom
// ============================================================================

TEST(QbToNumberPrefix, IntPrefixTolerance) {
    std::size_t consumed = 0;
    EXPECT_EQ(to_number_prefix<int>("12abc", &consumed), 12);
    EXPECT_EQ(consumed, 2u);

    EXPECT_EQ(to_number_prefix<int>("  42", &consumed), 42);
    EXPECT_EQ(consumed, 4u); // 2 spaces + "42"

    EXPECT_EQ(to_number_prefix<int>("+7", &consumed), 7);
    EXPECT_EQ(consumed, 2u); // '+' + "7"

    EXPECT_EQ(to_number_prefix<int>("-7"), -7);
    EXPECT_EQ(to_number_prefix<long long>("100 200", &consumed), 100);
    EXPECT_EQ(consumed, 3u);
}

TEST(QbToNumberPrefix, IntPrefixFailureAndRange) {
    EXPECT_FALSE(to_number_prefix<int>("").has_value());
    EXPECT_FALSE(to_number_prefix<int>("abc").has_value());
    EXPECT_FALSE(to_number_prefix<int>("   ").has_value()); // ws only, no digits
    // overflow of the magnitude -> nullopt (std::stoi would throw out_of_range).
    EXPECT_FALSE(to_number_prefix<std::int16_t>("99999").has_value());
}

TEST(QbToNumberPrefix, FloatPrefixTolerance) {
    std::size_t consumed = 0;
    EXPECT_DOUBLE_EQ(*to_number_prefix<double>("2.5 extra", &consumed), 2.5);
    EXPECT_EQ(consumed, 3u);
    EXPECT_FLOAT_EQ(*to_number_prefix<float>("1.5xyz"), 1.5f);
    // subnormal still parses in prefix mode (no throw).
    EXPECT_TRUE(to_number_prefix<double>("5e-324").has_value());
    // overflow -> nullopt.
    EXPECT_FALSE(to_number_prefix<double>("1e400").has_value());
}

TEST(QbToNumberPrefix, PrefixConsumedNullptrSafe) {
    // consumed defaulting to nullptr must not crash.
    EXPECT_EQ(to_number_prefix<int>("55"), 55);
    EXPECT_DOUBLE_EQ(*to_number_prefix<double>("0.25"), 0.25);
}

TEST(QbToNumberPrefix, PrefixHexBase) {
    std::size_t consumed = 0;
    EXPECT_EQ(to_number_prefix<unsigned>("ffg", &consumed, 16), 0xFFu);
    EXPECT_EQ(consumed, 2u); // "ff", 'g' is not a hex digit
}
