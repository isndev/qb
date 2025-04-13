/**
 * @file test-timestamp.cpp
 * @brief Unit tests for qb timestamp system
 *
 * This file contains unit tests for the timestamp system, including
 * Duration and TimePoint classes and their derivatives.
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
 */

#include <gtest/gtest.h>
#include <qb/system/timestamp.h>
#include <chrono>
#include <thread>

namespace {

// Test fixture for Duration tests
class DurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for duration tests
    }
};

// Test fixture for TimePoint tests
class TimePointTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Common setup for time point tests
    }
};

// Duration tests

TEST_F(DurationTest, DefaultConstruction) {
    qb::Duration d;
    EXPECT_EQ(d.count(), 0);
    EXPECT_EQ(d.nanoseconds(), 0);
}

TEST_F(DurationTest, ExplicitConstruction) {
    qb::Duration d(1000000000); // 1 second in nanoseconds
    EXPECT_EQ(d.count(), 1000000000);
    EXPECT_EQ(d.seconds(), 1);
    EXPECT_EQ(d.milliseconds(), 1000);
    EXPECT_EQ(d.microseconds(), 1000000);
    EXPECT_EQ(d.nanoseconds(), 1000000000);
}

TEST_F(DurationTest, FromMethods) {
    // Test factory methods
    auto d1 = qb::Duration::from_days(1);
    EXPECT_EQ(d1.days(), 1);
    EXPECT_EQ(d1.hours(), 24);
    
    auto d2 = qb::Duration::from_hours(2);
    EXPECT_EQ(d2.hours(), 2);
    EXPECT_EQ(d2.minutes(), 120);
    
    auto d3 = qb::Duration::from_minutes(3);
    EXPECT_EQ(d3.minutes(), 3);
    EXPECT_EQ(d3.seconds(), 180);
    
    auto d4 = qb::Duration::from_seconds(4);
    EXPECT_EQ(d4.seconds(), 4);
    EXPECT_EQ(d4.milliseconds(), 4000);
    
    auto d5 = qb::Duration::from_milliseconds(5);
    EXPECT_EQ(d5.milliseconds(), 5);
    EXPECT_EQ(d5.microseconds(), 5000);
    
    auto d6 = qb::Duration::from_microseconds(6);
    EXPECT_EQ(d6.microseconds(), 6);
    EXPECT_EQ(d6.nanoseconds(), 6000);
    
    auto d7 = qb::Duration::from_nanoseconds(7);
    EXPECT_EQ(d7.nanoseconds(), 7);
}

TEST_F(DurationTest, ArithmeticOperations) {
    qb::Duration d1(1000000000); // 1s
    qb::Duration d2(500000000);  // 0.5s
    
    // Addition
    auto d3 = d1 + d2;
    EXPECT_EQ(d3.nanoseconds(), 1500000000);
    EXPECT_EQ(d3.milliseconds(), 1500);
    
    // Subtraction
    auto d4 = d1 - d2;
    EXPECT_EQ(d4.nanoseconds(), 500000000);
    EXPECT_EQ(d4.milliseconds(), 500);
    
    // Compound assignment
    d1 += d2;
    EXPECT_EQ(d1.nanoseconds(), 1500000000);
    
    d1 -= d2;
    EXPECT_EQ(d1.nanoseconds(), 1000000000);
    
    // Multiplication
    auto d5 = d2 * 2;
    EXPECT_EQ(d5.nanoseconds(), 1000000000);
    
    auto d6 = 3 * d2;
    EXPECT_EQ(d6.nanoseconds(), 1500000000);
    
    // Division
    auto d7 = d1 / 2;
    EXPECT_EQ(d7.nanoseconds(), 500000000);
    
    // Unary operations
    auto d8 = +d1;
    EXPECT_EQ(d8.nanoseconds(), 1000000000);
    
    auto d9 = -d1;
    EXPECT_EQ(d9.nanoseconds(), -1000000000);
}

TEST_F(DurationTest, ComparisonOperations) {
    qb::Duration d1(1000000000); // 1s
    qb::Duration d2(500000000);  // 0.5s
    qb::Duration d3(1000000000); // 1s
    
    // Equality
    EXPECT_TRUE(d1 == d3);
    EXPECT_FALSE(d1 == d2);
    
    // Inequality
    EXPECT_TRUE(d1 != d2);
    EXPECT_FALSE(d1 != d3);
    
    // Less than
    EXPECT_TRUE(d2 < d1);
    EXPECT_FALSE(d1 < d2);
    
    // Greater than
    EXPECT_TRUE(d1 > d2);
    EXPECT_FALSE(d2 > d1);
    
    // Less than or equal
    EXPECT_TRUE(d2 <= d1);
    EXPECT_TRUE(d1 <= d3);
    EXPECT_FALSE(d1 <= d2);
    
    // Greater than or equal
    EXPECT_TRUE(d1 >= d2);
    EXPECT_TRUE(d1 >= d3);
    EXPECT_FALSE(d2 >= d1);
}

TEST_F(DurationTest, ChromoConversion) {
    // From standard chrono
    std::chrono::seconds sec(5);
    qb::Duration d1(sec);
    EXPECT_EQ(d1.seconds(), 5);
    
    std::chrono::milliseconds ms(100);
    qb::Duration d2(ms);
    EXPECT_EQ(d2.milliseconds(), 100);
    
    // To standard chrono
    auto d3 = qb::Duration::from_minutes(2);
    auto std_minutes = d3.to<std::chrono::minutes>();
    EXPECT_EQ(std_minutes.count(), 2);
    
    auto d4 = qb::Duration::from_seconds(30);
    auto std_sec = d4.to_chrono();
    EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(std_sec).count(), 30);
}

// TimePoint tests

TEST_F(TimePointTest, DefaultConstruction) {
    qb::TimePoint tp;
    EXPECT_EQ(tp.count(), 0);
}

TEST_F(TimePointTest, ExplicitConstruction) {
    qb::TimePoint tp(1000000000); // 1 second after epoch
    EXPECT_EQ(tp.seconds(), 1);
    EXPECT_EQ(tp.milliseconds(), 1000);
}

TEST_F(TimePointTest, Now) {
    auto now1 = qb::TimePoint::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto now2 = qb::TimePoint::now();
    
    EXPECT_GT(now2, now1);
    auto diff = now2 - now1;
    EXPECT_GE(diff.milliseconds(), 10);
}

TEST_F(TimePointTest, FromMethods) {
    auto tp1 = qb::TimePoint::from_days(1);
    EXPECT_EQ(tp1.days(), 1);
    
    auto tp2 = qb::TimePoint::from_hours(2);
    EXPECT_EQ(tp2.hours(), 2);
    
    auto tp3 = qb::TimePoint::from_minutes(3);
    EXPECT_EQ(tp3.minutes(), 3);
    
    auto tp4 = qb::TimePoint::from_seconds(4);
    EXPECT_EQ(tp4.seconds(), 4);
    
    auto tp5 = qb::TimePoint::from_milliseconds(5);
    EXPECT_EQ(tp5.milliseconds(), 5);
    
    auto tp6 = qb::TimePoint::from_microseconds(6);
    EXPECT_EQ(tp6.microseconds(), 6);
    
    auto tp7 = qb::TimePoint::from_nanoseconds(7);
    EXPECT_EQ(tp7.nanoseconds(), 7);
    
    // Epoch should be 0
    EXPECT_EQ(qb::TimePoint::epoch().count(), 0);
}

TEST_F(TimePointTest, ArithmeticOperations) {
    qb::TimePoint tp1(1000000000); // 1s after epoch
    qb::Duration d1(500000000);    // 0.5s
    
    // Addition
    auto tp2 = tp1 + d1;
    EXPECT_EQ(tp2.nanoseconds(), 1500000000);
    
    auto tp3 = d1 + tp1;
    EXPECT_EQ(tp3.nanoseconds(), 1500000000);
    
    // Subtraction
    auto tp4 = tp1 - d1;
    EXPECT_EQ(tp4.nanoseconds(), 500000000);
    
    // Duration between time points
    auto d2 = tp2 - tp1;
    EXPECT_EQ(d2.nanoseconds(), 500000000);
    
    // Compound assignment
    tp1 += d1;
    EXPECT_EQ(tp1.nanoseconds(), 1500000000);
    
    tp1 -= d1;
    EXPECT_EQ(tp1.nanoseconds(), 1000000000);
}

TEST_F(TimePointTest, ComparisonOperations) {
    qb::TimePoint tp1(1000000000); // 1s after epoch
    qb::TimePoint tp2(500000000);  // 0.5s after epoch
    qb::TimePoint tp3(1000000000); // 1s after epoch
    
    // Equality
    EXPECT_TRUE(tp1 == tp3);
    EXPECT_FALSE(tp1 == tp2);
    
    // Inequality
    EXPECT_TRUE(tp1 != tp2);
    EXPECT_FALSE(tp1 != tp3);
    
    // Less than
    EXPECT_TRUE(tp2 < tp1);
    EXPECT_FALSE(tp1 < tp2);
    
    // Greater than
    EXPECT_TRUE(tp1 > tp2);
    EXPECT_FALSE(tp2 > tp1);
    
    // Less than or equal
    EXPECT_TRUE(tp2 <= tp1);
    EXPECT_TRUE(tp1 <= tp3);
    EXPECT_FALSE(tp1 <= tp2);
    
    // Greater than or equal
    EXPECT_TRUE(tp1 >= tp2);
    EXPECT_TRUE(tp1 >= tp3);
    EXPECT_FALSE(tp2 >= tp1);
}

TEST_F(TimePointTest, ChromoConversion) {
    // From standard chrono
    auto std_now = std::chrono::system_clock::now();
    qb::TimePoint tp1(std_now);
    
    // To standard chrono
    auto tp2 = qb::TimePoint::from_seconds(60);
    auto std_tp = tp2.to_chrono();
    auto std_sec = std::chrono::time_point_cast<std::chrono::seconds>(std_tp);
    EXPECT_EQ(std_sec.time_since_epoch().count(), 60);
    
    auto std_sys_tp = tp2.to<std::chrono::system_clock>();
    EXPECT_EQ(std::chrono::time_point_cast<std::chrono::seconds>(std_sys_tp).time_since_epoch().count(), 60);
}

TEST_F(TimePointTest, Formatting) {
    // Create a time point at 2023-01-15T12:30:45Z
    std::tm tm = {};
    tm.tm_year = 2023 - 1900; // Years since 1900
    tm.tm_mon = 0;            // Month (0-11)
    tm.tm_mday = 15;          // Day (1-31)
    tm.tm_hour = 12;          // Hour (0-23)
    tm.tm_min = 30;           // Minute (0-59)
    tm.tm_sec = 45;           // Second (0-59)
    tm.tm_isdst = 0;          // DST flag (0 = not in effect)
    
    std::time_t time_t_value = std::mktime(&tm);
    qb::TimePoint tp(static_cast<qb::TimePoint::rep>(time_t_value) * 1000000000ULL);
    
    // Test different formats
    std::string iso_format = tp.to_iso8601();
    EXPECT_TRUE(iso_format.find("2023") != std::string::npos);
    
    std::string custom_format = tp.format("%Y-%m-%d");
    EXPECT_EQ(custom_format, "2023-01-15");
}

// Specialized TimePoint tests

TEST_F(TimePointTest, SpecializedTimePoints) {
    // Test that all specialized time points can be constructed
    qb::UtcTimePoint utp;
    qb::LocalTimePoint ltp;
    qb::HighResTimePoint hrtp;
    qb::TscTimePoint tscp;
    
    // Test their .now() methods
    auto utp_now = qb::UtcTimePoint::now();
    auto ltp_now = qb::LocalTimePoint::now();
    auto hrtp_now = qb::HighResTimePoint::now();
    auto tscp_now = qb::TscTimePoint::now();
    
    // They should all be close to each other
    EXPECT_NEAR(static_cast<double>(utp_now.seconds()),
                static_cast<double>(hrtp_now.seconds()),
                1.0); // 1 second tolerance
}

// Timer tests

TEST_F(TimePointTest, ScopedTimer) {
    bool callback_invoked = false;
    qb::Duration measured_duration;
    
    {
        // Create a scoped timer with a callback
        qb::ScopedTimer timer([&callback_invoked, &measured_duration](qb::Duration duration) {
            callback_invoked = true;
            measured_duration = duration;
        });
        
        // Sleep to ensure measurable duration
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Test elapsed without stopping
        qb::Duration elapsed = timer.elapsed();
        EXPECT_GE(elapsed.milliseconds(), 50);
    } // Scope end should trigger callback
    
    EXPECT_TRUE(callback_invoked);
    EXPECT_GE(measured_duration.milliseconds(), 50);
}

TEST_F(TimePointTest, LogTimer) {
    qb::LogTimer timer("Test timer");
    
    // Sleep to ensure measurable duration
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Test elapsed
    qb::Duration elapsed = timer.elapsed();
    EXPECT_GE(elapsed.milliseconds(), 10);
    
    // Note: We can't easily test the automatic logging behavior in a unit test
}

// Legacy API compatibility tests

TEST_F(TimePointTest, LegacyCompatibility) {
    // Test that the old names still work
    qb::Timespan ts(1000000000); // 1s
    EXPECT_EQ(ts.seconds(), 1);
    
    qb::Timestamp tp(2000000000); // 2s after epoch
    EXPECT_EQ(tp.seconds(), 2);
    
    qb::UtcTimestamp utp;
    qb::LocalTimestamp ltp;
    qb::NanoTimestamp ntp;
    qb::RdtsTimestamp rtp;
    
    // Test that arithmetic between old and new types works
    qb::Duration d(500000000); // 0.5s
    qb::TimePoint timepoint(1000000000); // 1s after epoch
    
    auto result1 = timepoint + ts;
    EXPECT_EQ(result1.seconds(), 2);
    
    auto result2 = tp - d;
    EXPECT_EQ(result2.seconds_float(), 1.5);
}

// Additional tests

TEST_F(DurationTest, LiteralsTest) {
    using namespace qb::literals;
    
    auto d1 = 5_d; // 5 days
    EXPECT_EQ(d1.days(), 5);
    
    auto d2 = 6_h; // 6 hours
    EXPECT_EQ(d2.hours(), 6);
    
    auto d3 = 7_min; // 7 minutes
    EXPECT_EQ(d3.minutes(), 7);
    
    auto d4 = 8_s; // 8 seconds
    EXPECT_EQ(d4.seconds(), 8);
    
    auto d5 = 9_ms; // 9 milliseconds
    EXPECT_EQ(d5.milliseconds(), 9);
    
    auto d6 = 10_us; // 10 microseconds
    EXPECT_EQ(d6.microseconds(), 10);
    
    auto d7 = 11_ns; // 11 nanoseconds
    EXPECT_EQ(d7.nanoseconds(), 11);
}

TEST_F(TimePointTest, ParsingFromStringTest) {
    auto tp1 = qb::TimePoint::from_iso8601("2023-01-15T12:30:45Z");
    EXPECT_TRUE(tp1.has_value());
    
    auto tp2 = qb::TimePoint::parse("2023/01/15 12:30:45", "%Y/%m/%d %H:%M:%S");
    EXPECT_TRUE(tp2.has_value());
    
    // Invalid formats should return nullopt
    auto tp3 = qb::TimePoint::from_iso8601("invalid-date");
    EXPECT_FALSE(tp3.has_value());
    
    auto tp4 = qb::TimePoint::parse("invalid-date", "%Y-%m-%d");
    EXPECT_FALSE(tp4.has_value());
}

} // namespace 