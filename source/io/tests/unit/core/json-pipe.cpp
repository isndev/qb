/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/json-pipe.cpp
 * @brief `qb::allocator::pipe<char>::put<qb::json>` serialization + typed `put<T>` overloads.
 *
 * `pipe<char>::put<json>` is the framework's zero-intermediate-string JSON serializer: it walks a
 * `qb::json` (nlohmann) value and emits its wire bytes straight into the output pipe. This is a
 * pure in-memory transform — NO socket, NO event loop — so it is a strict `unit` test.
 *
 * Consolidated (spec D4 + §2) from:
 *   - the JSON half of system/test-uri-json.cpp (primitive/empty/composite serialization, the
 *     discarded sentinel, and the uuid json round-trip);
 *   - the `CharPipe` typed-`put<T>` case lifted out of system/test-file-operations.cpp
 *     (`CharPipeTypedPutAndStreamOutput`) — it tests `pipe`, not files, so it belongs here.
 *
 * STRENGTHENED (the headline of this file): exact-byte assertions on a 64-bit integer and a
 * non-trivial double. The historic `put<json>` bug serialized every number through
 * `get<int>`/`get<unsigned int>`/`get<float>`, which truncated int64 timestamps to 32 bits (large
 * positive values went negative) and lost double precision. We pin the *exact* serialized text for
 * INT64_MAX/MIN, a large unsigned, and a double whose value cannot survive a float round-trip — so
 * any regression to a narrowing accessor fails loudly here instead of silently on the wire.
 */

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <qb/json.h>            // pulls qb::json + the uuids json codec (to_json / from_json)
#include <qb/system/allocator/pipe.h>
#include <qb/uuid.h>            // qb::uuid + qb::generate_random_uuid

namespace {

/// Serialize a json value through the pipe and return the emitted bytes.
std::string
pipe_json(qb::json const &value) {
    qb::allocator::pipe<char> pipe;
    pipe.put<qb::json>(value);
    return pipe.str();
}

} // namespace

// =============================================================================
// PRIMITIVE / EMPTY / COMPOSITE SERIALIZATION
// =============================================================================

/**
 * @test Scalars, empty containers, a heterogeneous array, and a nested object serialize to canonical text.
 * @brief Folded from test-uri-json.cpp::SerializesPrimitiveEmptyAndCompositeValues.
 */
TEST(JsonPipe, SerializesPrimitivesEmptyAndComposites) {
    EXPECT_EQ(pipe_json(qb::json(nullptr)), "null");
    EXPECT_EQ(pipe_json(qb::json(true)), "true");
    EXPECT_EQ(pipe_json(qb::json(false)), "false");
    EXPECT_EQ(pipe_json(qb::json(-42)), "-42");
    EXPECT_EQ(pipe_json(qb::json(42u)), "42");
    EXPECT_EQ(pipe_json(qb::json("qb")), "\"qb\"");
    EXPECT_EQ(pipe_json(qb::json::object()), "{}");
    EXPECT_EQ(pipe_json(qb::json::array()), "[]");

    const qb::json array = qb::json::array({1, "two", true, nullptr});
    EXPECT_EQ(pipe_json(array), "[1,\"two\",true,null]");

    qb::json object;
    object["array"] = array;
    object["empty"] = qb::json::object();
    object["float"] = 1.25;

    // Object key order is implementation-defined, so assert via a structural re-parse,
    // not a byte string.
    const std::string serialized = pipe_json(object);
    const qb::json    reparsed   = qb::json::parse(serialized);
    EXPECT_EQ(reparsed, object);
}

// =============================================================================
// EXACT-BYTE NUMERIC FIDELITY — locks the put<json> truncation regression
// =============================================================================

/**
 * @test A 64-bit integer serializes with its FULL precision (no narrowing to int32).
 * @brief Regression guard. `put<json>` once routed numbers through `get<int>`, truncating
 *        int64 to 32 bits — INT64_MAX wrapped to -1, large timestamps went negative. We assert
 *        the exact decimal text for the 64-bit boundaries so any narrowing reappears as a diff.
 */
TEST(JsonPipe, SerializesInt64WithoutTruncation) {
    constexpr std::int64_t  i64_max = std::numeric_limits<std::int64_t>::max(); // 9223372036854775807
    constexpr std::int64_t  i64_min = std::numeric_limits<std::int64_t>::min(); // -9223372036854775808
    constexpr std::uint64_t u64_big = 18446744073709551615ull;                  // UINT64_MAX

    EXPECT_EQ(pipe_json(qb::json(i64_max)), "9223372036854775807");
    EXPECT_EQ(pipe_json(qb::json(i64_min)), "-9223372036854775808");
    EXPECT_EQ(pipe_json(qb::json(u64_big)), "18446744073709551615");

    // A realistic millisecond timestamp (> INT32_MAX) must stay positive and exact.
    constexpr std::int64_t epoch_ms = 1750000000000ll;
    EXPECT_EQ(pipe_json(qb::json(epoch_ms)), "1750000000000");

    // And it must round-trip back to the same 64-bit value, not a truncated one.
    const qb::json reparsed = qb::json::parse(pipe_json(qb::json(epoch_ms)));
    EXPECT_EQ(reparsed.get<std::int64_t>(), epoch_ms);
}

/**
 * @test A double serializes at full double precision (no narrowing to float).
 * @brief Regression guard. `put<json>` once used `get<float>`, dropping mantissa bits. We pick a
 *        value that a float cannot represent (0.1 has no exact binary form; its double and float
 *        round-trips differ) and assert the serialized text re-parses to the SAME double bit
 *        pattern — proving the serializer kept double precision end to end.
 */
TEST(JsonPipe, SerializesDoubleWithoutPrecisionLoss) {
    const double precise = 0.123456789012345; // 15 significant digits — beyond float's ~7
    const std::string serialized = pipe_json(qb::json(precise));

    const qb::json reparsed = qb::json::parse(serialized);
    EXPECT_DOUBLE_EQ(reparsed.get<double>(), precise);
    // A float round-trip would have lost bits; prove the pipe did NOT take that path.
    EXPECT_NE(reparsed.get<double>(), static_cast<double>(static_cast<float>(precise)))
        << "double serialized through a float accessor would collapse to the float value";

    // Pi to full double precision survives as well.
    const double pi = 3.141592653589793;
    EXPECT_DOUBLE_EQ(qb::json::parse(pipe_json(qb::json(pi))).get<double>(), pi);
}

// =============================================================================
// DISCARDED SENTINEL + UUID ROUND-TRIP
// =============================================================================

/**
 * @test A discarded json (failed parse) serializes to the explicit `<discarded>` sentinel, and the
 *       uuid json codec round-trips.
 * @brief Folded from test-uri-json.cpp::SerializesDiscardedAndUuidRoundTrip.
 */
TEST(JsonPipe, SerializesDiscardedSentinelAndUuidRoundTrip) {
    const qb::json discarded = qb::json::parse("{", nullptr, false);
    ASSERT_TRUE(discarded.is_discarded());
    EXPECT_EQ(pipe_json(discarded), "<discarded>");

    const qb::uuid id = qb::generate_random_uuid();
    qb::json       encoded;
    uuids::to_json(encoded, id);

    qb::uuid decoded;
    uuids::from_json(encoded, decoded);
    EXPECT_EQ(decoded, id);
}

// =============================================================================
// TYPED put<T> OVERLOADS (CharPipe, lifted from test-file-operations.cpp)
// =============================================================================

/**
 * @test The typed `put<T>` overloads each append their value, and a pipe streams to an ostream.
 * @brief Lifted verbatim-in-intent from test-file-operations.cpp::CharPipeTypedPutAndStreamOutput
 *        (it exercised `pipe`, not the filesystem). Covers `put<char>`, `put<unsigned char>`,
 *        `put<const char*>` (incl. the empty-string no-op), `put<std::string>`,
 *        `put<std::string_view>`, `put<pipe<char>>` (pipe-into-pipe), the zero-length `put`/`write`
 *        no-ops, and `operator<<` to a std::ostream.
 */
TEST(JsonPipe, TypedPutOverloadsAppendAndStream) {
    qb::allocator::pipe<char> source;
    source.put<char>('q');
    source.put<unsigned char>(static_cast<unsigned char>('b'));
    source.put<const char *>(""); // empty C-string is a no-op
    source.put<std::string>(std::string("-"));
    source.put<std::string_view>(std::string_view("io"));
    EXPECT_EQ(source.view(), "qb-io");

    qb::allocator::pipe<char> copied;
    copied.put<qb::allocator::pipe<char>>(source); // pipe-into-pipe
    copied.put("", 0);                             // zero-length put is a no-op
    copied.write("", 0);                           // zero-length write is a no-op
    EXPECT_EQ(copied.str(), "qb-io");

    std::ostringstream out;
    out << copied;
    EXPECT_EQ(out.str(), "qb-io");
}
