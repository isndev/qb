/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/stream/stream-limits.cpp
 * @brief `qb::io::stream<>` read/write buffer-size limits — defaults, accessors, publish cap.
 *
 * `qb::io::stream<_Transport>` (qb/io/stream.h) caps how much it will buffer in each direction so a
 * stalled peer cannot make it allocate without bound. This test pins that policy on a stream
 * templated over `tcp::socket` WITHOUT ever opening the socket — only the buffer-limit accessors and
 * `publish()`'s cap check are exercised, all in memory — so it is a strict `unit` test (no bind, no
 * connect, no event loop).
 *
 * Migrated from system/test-io.cpp::StreamLimits.* (spec §2). Coverage: the default limits match the
 * `QB_MAX_*_BUFFER_SIZE` macros and are a sane non-degenerate range; `set_max_*` round-trips; and
 * `publish()` accepts up to (and exactly at) the cap but rejects (returns nullptr, leaves the queue
 * unchanged) the byte that would cross it.
 */

#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include <qb/io/stream.h>
#include <qb/io/tcp/socket.h>

using stream_t = qb::io::stream<qb::io::tcp::socket>;

// =============================================================================
// DEFAULTS / ACCESSORS
// =============================================================================

/**
 * @test The default read/write buffer limits equal the configured macros and bracket a sane range.
 * @brief Folded from StreamLimits.DefaultBufferLimitsAreConfigured. The `> 0` and `< SIZE_MAX`
 *        bounds pin that the default is a real cap, not 0 (reject-everything) or unbounded.
 */
TEST(StreamLimits, DefaultBufferLimitsAreConfigured) {
    stream_t s;
    EXPECT_EQ(s.max_read_buffer_size(), QB_MAX_READ_BUFFER_SIZE);
    EXPECT_EQ(s.max_write_buffer_size(), QB_MAX_WRITE_BUFFER_SIZE);
    EXPECT_GT(s.max_read_buffer_size(), 0u);
    EXPECT_LT(s.max_read_buffer_size(), std::numeric_limits<std::size_t>::max());
}

/**
 * @test `set_max_read_buffer_size` / `set_max_write_buffer_size` round-trip.
 * @brief Folded from StreamLimits.SetMaxBufferSizes.
 */
TEST(StreamLimits, SetMaxBufferSizesRoundTrip) {
    stream_t s;

    s.set_max_read_buffer_size(1024);
    EXPECT_EQ(s.max_read_buffer_size(), 1024u);

    s.set_max_write_buffer_size(2048);
    EXPECT_EQ(s.max_write_buffer_size(), 2048u);
}

// =============================================================================
// publish — write-buffer cap
// =============================================================================

/**
 * @test `publish()` rejects the write that would exceed the cap and leaves the queue unchanged.
 * @brief Folded from StreamLimits.PublishRejectsWhenLimitExceeded. Two accepted publishes fill the
 *        buffer exactly to the 20-byte cap; the third (even 1 byte) is rejected — returns nullptr
 *        and `pendingWrite()` does not grow.
 */
TEST(StreamLimits, PublishRejectsWhenLimitWouldBeExceeded) {
    stream_t s;
    s.set_max_write_buffer_size(20);

    const char data[] = "1234567890";
    EXPECT_NE(s.publish(data, 10), nullptr);
    EXPECT_EQ(s.pendingWrite(), 10u);

    EXPECT_NE(s.publish(data, 10), nullptr);
    EXPECT_EQ(s.pendingWrite(), 20u);

    // One more byte would cross the 20-byte cap → rejected, queue unchanged.
    EXPECT_EQ(s.publish(data, 1), nullptr);
    EXPECT_EQ(s.pendingWrite(), 20u);
}

/**
 * @test `publish()` accepts a write that fills the buffer EXACTLY to the cap.
 * @brief Folded from StreamLimits.PublishAcceptsExactLimit — the at-boundary accept.
 */
TEST(StreamLimits, PublishAcceptsExactLimit) {
    stream_t s;
    s.set_max_write_buffer_size(10);

    const char data[] = "1234567890";
    EXPECT_NE(s.publish(data, 10), nullptr) << "filling exactly to the cap must be accepted";
    EXPECT_EQ(s.pendingWrite(), 10u);
}
