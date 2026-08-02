/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/protocol/json-depth-guard.cpp
 * @brief JSON / MessagePack nesting-depth DoS guard (`qb::protocol::detail`).
 *
 * The json protocols (qb/io/protocol/json.h) run a recursive-descent parser, so a payload of
 * thousands of nested `[`/`{` would exhaust the C++ stack — a stack overflow no try/catch can
 * recover — before any message-size limit applies. Two linear pre-scans bound the nesting *before*
 * the recursive reader runs: `json_depth_within` (a string-aware byte scan for text JSON) and
 * `msgpack_depth_within` (a SAX-driven scan for MessagePack, built on `msgpack_depth_sax`). These
 * are pure functions over a byte buffer — NO socket, NO event loop — a strict `unit` test.
 *
 * Migrated from system/test-io.cpp::JsonProtocol.* (spec §2/D4). The boundary contracts are pinned
 * exactly: nesting inside string literals (and across escaped quotes) does NOT count toward depth;
 * a buffer exactly at `kJsonMaxNestingDepth` is accepted while one level over is rejected; and the
 * `msgpack_depth_sax` scalar callbacks all accept while a depth-1 sax rejects the second container.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/protocol/json.h>

// =============================================================================
// json_depth_within — string-aware byte pre-scan for text JSON
// =============================================================================

/**
 * @test Reasonable nesting passes; brackets inside string literals are ignored; the limit boundary
 *       (exactly-at vs one-over) is exact.
 * @brief Folded from JsonProtocol.DepthGuard. The string-awareness (and escaped-quote handling) is
 *        the security-critical part — a naive bracket counter would over-count `"[[[[" and falsely
 *        reject benign payloads.
 */
namespace {
/// A byte literal above 0x7F does not fit a signed `char`, so a direct `char(0x91)`
/// is a truncating cast (MSVC C4310). Rounding through `unsigned char` is the
/// well-defined spelling and says "this is a byte", which is what these tests mean.
constexpr char
byte_c(unsigned v) noexcept {
    return static_cast<char>(static_cast<unsigned char>(v));
}
} // namespace

TEST(JsonDepthGuard, TextScanIsStringAwareAndBoundedExactly) {
    using qb::protocol::detail::json_depth_within;
    constexpr std::size_t kMax = qb::protocol::detail::kJsonMaxNestingDepth;

    // Reasonable nesting passes.
    const std::string ok = R"({"a":{"b":[1,2,{"c":3}]}})";
    EXPECT_TRUE(json_depth_within(ok.data(), ok.size(), kMax));

    // Brackets INSIDE a string literal must not count toward depth.
    const std::string in_string = R"({"k":"[[[[[[[[[[ not real nesting ]]]]]]]]]]"})";
    EXPECT_TRUE(json_depth_within(in_string.data(), in_string.size(), 4));

    // An escaped quote keeps the scanner in-string, so the brackets after it stay ignored.
    const std::string esc = R"({"k":"a\"[[[[[[ b"})";
    EXPECT_TRUE(json_depth_within(esc.data(), esc.size(), 2));

    // Pathological nesting beyond the limit is rejected.
    const std::string bomb(kMax + 5, '[');
    EXPECT_FALSE(json_depth_within(bomb.data(), bomb.size(), kMax));

    // Exactly at the limit is accepted; one level over is not.
    const std::string at_limit(kMax, '[');
    EXPECT_TRUE(json_depth_within(at_limit.data(), at_limit.size(), kMax));
    const std::string over(kMax + 1, '[');
    EXPECT_FALSE(json_depth_within(over.data(), over.size(), kMax));
}

// =============================================================================
// msgpack_depth_within — SAX-driven pre-scan for MessagePack
// =============================================================================

/**
 * @test Reasonable MessagePack nesting passes; the limit boundary is exact.
 * @brief Folded from JsonProtocol.MsgpackDepthGuard. `0x91` = fixarray(1 element); `0xc0` = nil. N
 *        nested single-element arrays give nesting depth N, which the pre-scan must bound before
 *        from_msgpack()'s recursive reader can blow the stack.
 */
TEST(JsonDepthGuard, MsgpackScanIsBoundedExactly) {
    using qb::protocol::detail::msgpack_depth_within;
    constexpr std::size_t kMax = qb::protocol::detail::kJsonMaxNestingDepth;

    // Reasonable nesting passes: [[[42]]].
    const std::string ok = {byte_c(0x91), byte_c(0x91), byte_c(0x91), byte_c(0x2a)};
    EXPECT_TRUE(msgpack_depth_within(ok.data(), ok.size(), kMax));

    // Pathological nesting beyond the limit is rejected.
    std::string bomb(kMax + 5, byte_c(0x91));
    bomb.push_back(byte_c(0xc0));
    EXPECT_FALSE(msgpack_depth_within(bomb.data(), bomb.size(), kMax));

    // Exactly at the limit is accepted; one level over is not.
    std::string at_limit(kMax, byte_c(0x91));
    at_limit.push_back(byte_c(0xc0));
    EXPECT_TRUE(msgpack_depth_within(at_limit.data(), at_limit.size(), kMax));
    std::string over(kMax + 1, byte_c(0x91));
    over.push_back(byte_c(0xc0));
    EXPECT_FALSE(msgpack_depth_within(over.data(), over.size(), kMax));
}

// =============================================================================
// msgpack_depth_sax — the SAX consumer's callbacks
// =============================================================================

/**
 * @test The SAX scalar callbacks all accept; a depth-1 sax accepts one container but rejects a
 *       nested second one; `parse_error` always returns false.
 * @brief Folded from JsonProtocol.MsgpackDepthSaxScalarCallbacksAndErrors. Drives the SAX interface
 *        directly (no DOM is built) so the depth bookkeeping and the no-op scalar handlers are each
 *        exercised on their own.
 */
TEST(JsonDepthGuard, MsgpackSaxScalarCallbacksAndDepthRejection) {
    qb::protocol::detail::msgpack_depth_sax scalar_sax(1);
    nlohmann::json::string_t                text = "qb";
    nlohmann::json::binary_t                binary(std::vector<std::uint8_t>{std::uint8_t{1}, std::uint8_t{2}});

    // Every scalar/key callback is a no-op that accepts.
    EXPECT_TRUE(scalar_sax.null());
    EXPECT_TRUE(scalar_sax.boolean(true));
    EXPECT_TRUE(scalar_sax.number_integer(-42));
    EXPECT_TRUE(scalar_sax.number_unsigned(42));
    EXPECT_TRUE(scalar_sax.number_float(1.25, text));
    EXPECT_TRUE(scalar_sax.string(text));
    EXPECT_TRUE(scalar_sax.binary(binary));
    EXPECT_TRUE(scalar_sax.key(text));

    // Depth-1 budget: the first container is accepted, a nested second is rejected.
    qb::protocol::detail::msgpack_depth_sax object_sax(1);
    EXPECT_TRUE(object_sax.start_object(0));
    EXPECT_FALSE(object_sax.start_array(0)) << "a second nested container exceeds the depth-1 budget";
    EXPECT_TRUE(object_sax.end_array());
    EXPECT_TRUE(object_sax.end_object());

    qb::protocol::detail::msgpack_depth_sax array_sax(1);
    EXPECT_TRUE(array_sax.start_array(0));
    EXPECT_FALSE(array_sax.start_object(0));
    EXPECT_TRUE(array_sax.end_object());
    EXPECT_TRUE(array_sax.end_array());

    // parse_error always aborts the scan.
    EXPECT_FALSE(scalar_sax.parse_error(0, "x", std::runtime_error("parse error")));
}
