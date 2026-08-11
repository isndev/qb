/**
 * @file unit/crypto/crypto-nossl-core.cpp
 * @brief The OpenSSL-free members of `qb::crypto` — must build and pass WITHOUT OpenSSL.
 *
 * `qb::crypto` is an OpenSSL facade with five exceptions: the hex codec
 * (`to_hex_string` / `hex_value` / `hex_to_string`), `xor_bytes` and
 * `constant_time_compare`. Those are plain C++ — a nibble loop over a digit table, a
 * 256-entry lookup, an XOR loop, a `volatile` accumulate — and they are declared and
 * defined (qb/source/io/src/crypto_core.cpp) whether or not OpenSSL is present, because
 * qbm-pgsql's `bytea` wire codec and the HPACK tests need them in a `QB_WITH_SSL=OFF`
 * build.
 *
 * This file is deliberately registered with **no `REQUIRES ssl`**: it is the only crypto
 * test that runs under the `feature-gates` preset. Every other crypto test carries
 * `REQUIRES ssl` and is therefore *not registered at all* when OpenSSL is absent, which
 * left the pure subset with zero coverage in exactly the configuration that depends on
 * it. It must not gain an `#ifdef QB_HAS_SSL` and must not use any other `qb::crypto`
 * member — under SSL-off the others do not exist and the file would stop compiling.
 *
 * The contracts proven:
 *   - `to_hex_string` is byte-exact for all 256 byte values, in both the upper-case and
 *     lower-case digit ranges, and defaults to upper-case;
 *   - `hex_value` returns the right nibble for all 22 hex digit characters and -1 for
 *     every one of the other 234 byte values (this is the 256-entry table that a
 *     transcription error would silently corrupt);
 *   - `hex_to_string` round-trips `to_hex_string` for all 256 bytes and for mixed case,
 *     and returns "" (parse failure, not garbage bytes) on odd length and on non-hex
 *     characters — including a non-hex digit in the low nibble only;
 *   - `xor_bytes` is involutive and throws `std::runtime_error` on size mismatch;
 *   - `constant_time_compare` honours equal / single-bit-differing / length-mismatch /
 *     both-empty.
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
 * @ingroup Tests
 */

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>

namespace {

/// Every byte value 0x00..0xFF, in order — the input the hex codec must survive intact.
std::string
all_bytes() {
    std::string s;
    s.reserve(256);
    for (int i = 0; i < 256; ++i)
        s.push_back(static_cast<char>(i));
    return s;
}

// -----------------------------------------------------------------------------
// to_hex_string
// -----------------------------------------------------------------------------

TEST(CryptoNoSslCore, ToHexStringCoversEveryByteInBothRanges) {
    const std::string input = all_bytes();

    const std::string upper = qb::crypto::to_hex_string(input, qb::crypto::range_hex_upper);
    const std::string lower = qb::crypto::to_hex_string(input, qb::crypto::range_hex_lower);

    ASSERT_EQ(upper.size(), 512u);
    ASSERT_EQ(lower.size(), 512u);

    // Independent re-derivation of the expected digits: do NOT reuse the codec's own
    // digit table, or a corrupted table would agree with itself.
    static constexpr std::string_view kUpper = "0123456789ABCDEF";
    static constexpr std::string_view kLower = "0123456789abcdef";
    for (int i = 0; i < 256; ++i) {
        const auto b = static_cast<unsigned char>(i);
        EXPECT_EQ(upper[static_cast<std::size_t>(i) * 2], kUpper[b >> 4]) << "byte " << i;
        EXPECT_EQ(upper[static_cast<std::size_t>(i) * 2 + 1], kUpper[b & 0x0F]) << "byte " << i;
        EXPECT_EQ(lower[static_cast<std::size_t>(i) * 2], kLower[b >> 4]) << "byte " << i;
        EXPECT_EQ(lower[static_cast<std::size_t>(i) * 2 + 1], kLower[b & 0x0F]) << "byte " << i;
    }
}

TEST(CryptoNoSslCore, ToHexStringDefaultsToUpperCaseAndHandlesEmpty) {
    EXPECT_EQ(qb::crypto::to_hex_string(std::string("\xDE\xAD\xBE\xEF", 4)), "DEADBEEF");
    EXPECT_EQ(qb::crypto::to_hex_string(std::string{}), "");
}

// -----------------------------------------------------------------------------
// hex_value — the 256-entry lookup table
// -----------------------------------------------------------------------------

TEST(CryptoNoSslCore, HexValueTableIsExactForAll256Entries) {
    // Expected table rebuilt from first principles, not from the range_hex_* constants.
    for (int i = 0; i < 256; ++i) {
        const auto c        = static_cast<unsigned char>(i);
        int        expected = -1;
        if (c >= '0' && c <= '9')
            expected = c - '0';
        else if (c >= 'a' && c <= 'f')
            expected = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            expected = c - 'A' + 10;

        EXPECT_EQ(qb::crypto::hex_value(c), expected) << "byte 0x" << std::hex << i;
    }
}

// -----------------------------------------------------------------------------
// hex_to_string
// -----------------------------------------------------------------------------

TEST(CryptoNoSslCore, HexRoundTripsEveryByteInBothCases) {
    const std::string input = all_bytes();

    EXPECT_EQ(qb::crypto::hex_to_string(qb::crypto::to_hex_string(input, qb::crypto::range_hex_upper)), input);
    EXPECT_EQ(qb::crypto::hex_to_string(qb::crypto::to_hex_string(input, qb::crypto::range_hex_lower)), input);
}

TEST(CryptoNoSslCore, HexToStringAcceptsMixedCase) {
    EXPECT_EQ(qb::crypto::hex_to_string("dEaDbEeF"), std::string("\xDE\xAD\xBE\xEF", 4));
}

TEST(CryptoNoSslCore, HexToStringRejectsOddLengthAndNonHex) {
    // NOTE on what these odd-length cases can and cannot prove. Deleting the `len & 1`
    // early return does NOT change any output here: the loop then runs `*it++` one past
    // end(), reads the NUL terminator, hex_value('\0') is -1, and the *non-hex* guard
    // returns "" anyway. The two guards are observationally redundant, so no black-box
    // assertion can distinguish them — `len & 1` earns its place by preventing the
    // out-of-bounds iterator read, not by changing the result. Verified by injection.
    EXPECT_EQ(qb::crypto::hex_to_string(""), "");
    EXPECT_EQ(qb::crypto::hex_to_string("A"), "");                   // odd length
    EXPECT_EQ(qb::crypto::hex_to_string("ABC"), "");                 // odd length
    EXPECT_EQ(qb::crypto::hex_to_string("ZZ"), "");                  // non-hex, high nibble
    EXPECT_EQ(qb::crypto::hex_to_string("AG"), "");                  // non-hex, LOW nibble only
    EXPECT_EQ(qb::crypto::hex_to_string("00 11"), "");               // embedded space (odd length too)
    EXPECT_EQ(qb::crypto::hex_to_string(std::string("A\0", 2)), ""); // embedded NUL
}

// -----------------------------------------------------------------------------
// xor_bytes
// -----------------------------------------------------------------------------

TEST(CryptoNoSslCore, XorBytesIsInvolutive) {
    const std::vector<unsigned char> a{0x00, 0xFF, 0x5A, 0xA5, 0x01};
    const std::vector<unsigned char> b{0xFF, 0xFF, 0x0F, 0xF0, 0x01};

    const auto x = qb::crypto::xor_bytes(a, b);
    EXPECT_EQ(x, (std::vector<unsigned char>{0xFF, 0x00, 0x55, 0x55, 0x00}));
    EXPECT_EQ(qb::crypto::xor_bytes(x, b), a);

    EXPECT_TRUE(qb::crypto::xor_bytes({}, {}).empty());
}

TEST(CryptoNoSslCore, XorBytesThrowsOnSizeMismatch) {
    EXPECT_THROW((void) qb::crypto::xor_bytes({0x01, 0x02}, {0x01}), std::runtime_error);
    EXPECT_THROW((void) qb::crypto::xor_bytes({0x01}, {}), std::runtime_error);
}

// -----------------------------------------------------------------------------
// constant_time_compare
// -----------------------------------------------------------------------------

TEST(CryptoNoSslCore, ConstantTimeCompareEqualDifferingAndLengthMismatch) {
    const std::vector<unsigned char> a{0xDE, 0xAD, 0xBE, 0xEF};

    EXPECT_TRUE(qb::crypto::constant_time_compare(a, a));
    EXPECT_TRUE(qb::crypto::constant_time_compare({}, {}));

    // Single-bit difference in the last byte — the case a short-circuiting memcmp
    // would still catch, but which pins that the volatile accumulate reaches the end.
    std::vector<unsigned char> b = a;
    b.back() ^= 0x01;
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, b));

    // Single-bit difference in the FIRST byte — pins that an early match is not
    // mistaken for equality.
    std::vector<unsigned char> c = a;
    c.front() ^= 0x80;
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, c));

    // Length mismatch, including the prefix case.
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, {0xDE, 0xAD, 0xBE}));
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, {}));
}

} // namespace
