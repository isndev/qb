/**
 * @file unit/crypto/crypto-reachable-edges.cpp
 * @brief `qb::crypto` reachable-but-unexercised edge paths — pure logic, no engine.
 *
 * The crypto-primitives / kdf-and-tokens / asymmetric-keys suites already pin the happy
 * paths and the OpenSSL-internal failure branches that survive in production are tagged
 * `LCOV_EXCL_LINE` (allocation / context-creation failures that cannot be provoked through
 * the public API with valid inputs). What remained genuinely reachable yet untested were a
 * handful of *input-driven* branches in `crypto.cpp` that the other suites step over because
 * their fixtures are too small or too well-formed to enter them. This file targets exactly
 * those, asserting observable behaviour (not merely "it ran"):
 *
 *   - PBKDF2 with a non-positive iteration count: `PKCS5_PBKDF2_HMAC_SHA1` rejects
 *     `iterations <= 0`, and `crypto::pbkdf2` is `noexcept` — so the documented failure
 *     contract is an *empty* string (the buffer is cleared rather than returned with
 *     uninitialised key material). The neighbouring Pbkdf2RejectsInvalidKeySizes test only
 *     drives the `key_size <= 0` guard, which short-circuits *before* PKCS5 is ever called;
 *     this drives the PKCS5-returns-failure clear path itself.
 *   - `generate_secure_random_string` rejection-sampling refill: with a character range
 *     whose size does not divide 256 the modulo-bias rejection loop discards bytes, and with
 *     a sufficiently long request the pre-drawn `len * 2` byte pool is exhausted and refilled
 *     mid-string. The neighbouring SecureRandomStringValidatesRangeAndLength test uses a
 *     2-char range (256 % 2 == 0 -> nothing is ever rejected -> the refill branch is dead),
 *     so this is the only test that enters the refill loop, while still proving the output
 *     stays in-range, has the requested length, and is non-degenerate.
 *   - `evp` chunked-stream digest loop: the streaming hash reads in `buffer_size` (128 KiB)
 *     chunks; inputs below that bound (every existing stream test uses a 20-byte string) run
 *     the read loop exactly once. A >128 KiB stream forces multiple `EVP_DigestUpdate` chunk
 *     iterations, and the result must still equal the one-shot string digest of the same
 *     bytes for MD5 / SHA-1 / SHA-256 / SHA-512 — a real divergence in the chunk-boundary
 *     handling would be caught here.
 *
 * No event loop, no socket, no daemon: a `unit` test (libcrypto is a link-time dependency).
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

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>

namespace {

/**
 * @brief Build a character range of the requested size from distinct, non-NUL bytes.
 *
 * A range size that is not a divisor of 256 makes the modulo-bias rejection threshold
 * `(256 / range_size) * range_size` strictly less than 256, so some random bytes are
 * rejected and the rejection-sampling loop in `generate_secure_random_string` actually
 * iterates. Size 129 in particular rejects roughly half of all draws (256 / 129 == 1,
 * threshold 129, so values 129..255 are discarded), which reliably exhausts the pre-drawn
 * `len * 2` byte pool on a long request and forces a refill.
 */
std::string
make_range(std::size_t size) {
    std::string range(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        // Start at 1 so the range never contains a NUL (keeps the set/in-range checks
        // unambiguous); for size <= 255 this stays within a single byte.
        range[i] = static_cast<char>(static_cast<unsigned char>(i + 1));
    }
    return range;
}

// =============================================================================
// PBKDF2 — PKCS5 failure path (non-positive iteration count)
// =============================================================================

TEST(CryptoReachableEdgesTest, Pbkdf2RejectsNonPositiveIterationCount) {
    // crypto::pbkdf2 is noexcept and forwards `iterations` straight to
    // PKCS5_PBKDF2_HMAC_SHA1, which fails for iterations <= 0. On that failure the
    // implementation must clear the key buffer and return an *empty* string rather than
    // expose the uninitialised buffer as key material. A positive key_size is used so the
    // earlier `key_size <= 0` guard does NOT short-circuit — the PKCS5 failure itself is
    // what we are exercising.
    EXPECT_NO_THROW({
        EXPECT_TRUE(qb::crypto::pbkdf2("password", "salt", 0, 16).empty());
        EXPECT_TRUE(qb::crypto::pbkdf2("password", "salt", -1, 16).empty());
        EXPECT_TRUE(qb::crypto::pbkdf2("password", "salt", -4096, 32).empty());
    });

    // A single positive iteration with the same positive key_size succeeds — this proves the
    // emptiness above is caused by the iteration count, not by the key_size or inputs.
    EXPECT_EQ(qb::crypto::pbkdf2("password", "salt", 1, 16).size(), 16u);
}

// =============================================================================
// generate_secure_random_string — rejection-sampling refill loop
// =============================================================================

TEST(CryptoReachableEdgesTest, SecureRandomStringRefillsPoolUnderBiasedRange) {
    // Range size 129 does not divide 256, so the rejection loop discards ~50% of draws.
    // A 5000-char request pre-draws 10000 bytes; with ~half rejected, the pool is exhausted
    // and refilled at least once mid-string — the path the 2-char-range neighbour can never
    // reach (256 % 2 == 0 -> zero rejections -> dead refill branch).
    const std::string range   = make_range(129);
    const std::size_t length  = 5000;
    const std::string result  = qb::crypto::generate_secure_random_string(length, range);

    // Length contract holds regardless of how many bytes were rejected/refilled.
    ASSERT_EQ(result.size(), length);

    // Every emitted character must come from the supplied range (rejection sampling must not
    // leak an out-of-range byte, e.g. one of the discarded 129..255 values).
    const std::set<char> allowed(range.begin(), range.end());
    for (const char c : result) {
        EXPECT_TRUE(allowed.count(c) == 1) << "emitted character outside the supplied range";
    }

    // The output must not be degenerate: a long draw over a 129-symbol alphabet should use a
    // large fraction of the alphabet. A stuck/biased generator (or a refill that re-seeds to a
    // constant) would collapse this count.
    const std::set<char> used(result.begin(), result.end());
    EXPECT_GT(used.size(), range.size() / 2);

    // Two independent long draws over the biased range must differ (the refill path is fed by
    // the CSPRNG, not a fixed buffer).
    EXPECT_NE(result, qb::crypto::generate_secure_random_string(length, range));
}

TEST(CryptoReachableEdgesTest, SecureRandomStringStaysInRangeForOddAlphabet) {
    // A small odd range (size 3: 256 % 3 != 0) also enters the rejection loop but is unlikely
    // to exhaust the pool at this length — this pins the in-range/length contract for the
    // common short-token case that still passes through the rejection comparison.
    const std::string range  = make_range(3);
    const std::string result = qb::crypto::generate_secure_random_string(256, range);

    ASSERT_EQ(result.size(), 256u);
    const std::set<char> allowed(range.begin(), range.end());
    for (const char c : result) {
        EXPECT_TRUE(allowed.count(c) == 1);
    }
    // All three symbols should appear across 256 draws over a 3-symbol alphabet.
    const std::set<char> used(result.begin(), result.end());
    EXPECT_EQ(used.size(), range.size());
}

// =============================================================================
// evp — chunked streaming digest loop over a > buffer_size input
// =============================================================================

TEST(CryptoReachableEdgesTest, StreamDigestMatchesOneShotForLargeMultiChunkInput) {
    // crypto::evp reads the stream in 128 KiB chunks; a 300 KiB input forces several
    // EVP_DigestUpdate iterations (the read `while` loop), whereas every existing stream test
    // uses a 20-byte string that runs the loop once. The chunked digest must byte-for-byte
    // equal the one-shot string digest of the same data for each algorithm — a chunk-boundary
    // bug (dropped/duplicated bytes, wrong gcount handling) would diverge here.
    std::string data(300u * 1024u, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>('A' + static_cast<int>(i % 26));
    }
    ASSERT_GT(data.size(), 131072u); // strictly larger than crypto's internal buffer_size

    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::md5(stream), qb::crypto::md5(data));
    }
    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::sha1(stream), qb::crypto::sha1(data));
    }
    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::sha256(stream), qb::crypto::sha256(data));
    }
    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::sha512(stream), qb::crypto::sha512(data));
    }
}

TEST(CryptoReachableEdgesTest, IteratedStreamDigestMatchesOneShotForLargeInput) {
    // The iterated stream overloads first run the multi-chunk evp() loop, then fold the digest
    // `iterations - 1` more times. Exercise that combination on a multi-chunk input and pin it
    // against the iterated string overload (which folds the same way over the one-shot digest).
    std::string data(200u * 1024u, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<unsigned char>((i * 31u + 7u) & 0xFFu));
    }
    ASSERT_GT(data.size(), 131072u);

    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::sha256(stream, 3), qb::crypto::sha256(data, 3));
    }
    {
        std::istringstream stream(data);
        EXPECT_EQ(qb::crypto::sha512(stream, 5), qb::crypto::sha512(data, 5));
    }
}

} // namespace
