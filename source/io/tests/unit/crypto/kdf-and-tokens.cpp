/**
 * @file unit/crypto/kdf-and-tokens.cpp
 * @brief `qb::crypto` KDF / token / metadata-envelope half — pure logic, no engine.
 *
 * This is the symmetric, key-derivation and secure-token half of the `qb::crypto`
 * facade (the asymmetric/PKI half lives in asymmetric-keys.cpp). Everything here is a
 * deterministic transform over in-memory buffers — HKDF, Argon2, the unified
 * `derive_key`, base64url, `generate_token`/`verify_token`, `encrypt_with_metadata`/
 * `decrypt_with_metadata`, `hash_password`/`verify_password`, and `generate_unique_iv`.
 * No event loop, no socket, no daemon: a `unit` test (libcrypto/Argon2 are link-time
 * dependencies, not runtime services). The contracts proven:
 *
 *   - HKDF matches the RFC 5869 SHA-256 known-answer vector and is digest-sensitive;
 *   - Argon2 is reproducible under a fixed salt, randomised under an implicit salt, and
 *     its three variants are pairwise distinct; hashlen 0 throws;
 *   - `derive_key` yields the requested length for PBKDF2/HKDF/Argon2 and the three
 *     algorithms disagree;
 *   - base64url is byte-exact (RFC alphabet, URL-safe, no padding) and round-trips;
 *   - secure tokens round-trip, reject the wrong key, reject malformed/truncated input,
 *     and a manufactured already-expired token (relative clock, NO sleep) fails to verify;
 *   - the metadata envelope authenticates payload + metadata, and fails on wrong key,
 *     tampered metadata, corrupted blob, mismatched algorithm and malformed JSON;
 *   - `hash_password` produces a verifiable, variant-tagged hash and rejects garbage;
 *   - `generate_unique_iv` is unique across 100 draws and supports short/empty sizes.
 *
 * Restructured from the dissolved system/test-crypto-advanced.cpp: the 2-second
 * `std::this_thread::sleep_for` TTL test is replaced by a token whose `exp` claim is
 * stamped in the past via the same epoch-seconds clock the verifier uses (deterministic,
 * zero wall-clock wait); the triple HKDF coverage (HKDF / HKDFWithDifferentDigests /
 * derive_key-HKDF) is collapsed into one digest-distinctness sweep plus the RFC vector;
 * the size-only Argon2/derive_key assertions are promoted to reproducibility + length
 * known-answer checks; the file-local main() is dropped for the shared gtest_main.
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

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>
#include <qb/json.h>

namespace {

/**
 * @brief Convert an even-length hex string to its raw bytes (test-vector helper).
 */
std::vector<unsigned char>
hex_to_bytes(const std::string &hex) {
    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<unsigned char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

/**
 * @brief Current Unix time in seconds — mirrors jwt/token internal clock so a claim can
 *        be stamped relative to "now" without ever sleeping.
 */
int64_t
unix_now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Fixture seeding deterministic-ish KDF inputs and a random AES-256-GCM key.
 */
class CryptoKdfAndTokensTest : public ::testing::Test {
protected:
    std::vector<unsigned char> test_input;
    std::vector<unsigned char> test_salt;
    std::vector<unsigned char> test_key;

    void
    SetUp() override {
        test_input = {'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
        test_salt  = qb::crypto::generate_salt(16);
        test_key   = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    }
};

// =============================================================================
// HKDF — RFC 5869 known-answer + digest-distinctness (triple coverage collapsed)
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, HkdfMatchesRfc5869Vector) {
    // RFC 5869, Appendix A.1 (Basic test case with SHA-256).
    const auto ikm          = hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const auto salt         = hex_to_bytes("000102030405060708090a0b0c");
    const auto info         = hex_to_bytes("f0f1f2f3f4f5f6f7f8f9");
    const auto expected_okm = hex_to_bytes("3cb25f25faacd57a90434f64d0362f2a"
                                           "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                           "34007208d5b887185865");

    const auto okm = qb::crypto::hkdf(ikm, salt, info, expected_okm.size(), qb::crypto::DigestAlgorithm::SHA256);
    EXPECT_EQ(okm, expected_okm);
}

TEST_F(CryptoKdfAndTokensTest, HkdfIsDigestSensitiveAndHandlesEmptyInputs) {
    const std::vector<unsigned char> input = {'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
    const std::vector<unsigned char> salt  = {'s', 'a', 'l', 't'};
    const std::vector<unsigned char> info  = {'i', 'n', 'f', 'o'};
    constexpr std::size_t            out   = 32;

    // The same IKM under different digests must yield pairwise-distinct OKMs.
    const std::vector<qb::crypto::DigestAlgorithm> digests = {
        qb::crypto::DigestAlgorithm::SHA1, qb::crypto::DigestAlgorithm::SHA256, qb::crypto::DigestAlgorithm::SHA384,
        qb::crypto::DigestAlgorithm::SHA512,
    };

    std::vector<std::vector<unsigned char>> results;
    for (const auto digest : digests) {
        const auto okm = qb::crypto::hkdf(input, salt, info, out, digest);
        EXPECT_EQ(okm.size(), out);
        results.push_back(okm);
    }
    for (std::size_t i = 0; i < results.size(); ++i) {
        for (std::size_t j = i + 1; j < results.size(); ++j) {
            EXPECT_NE(results[i], results[j]);
        }
    }

    // Empty info and empty salt are both accepted; empty salt changes the output.
    const auto with_salt    = qb::crypto::hkdf(input, salt, info, out, qb::crypto::DigestAlgorithm::SHA256);
    const auto empty_salt   = qb::crypto::hkdf(input, {}, info, out, qb::crypto::DigestAlgorithm::SHA256);
    const auto empty_info   = qb::crypto::hkdf(input, salt, {}, out, qb::crypto::DigestAlgorithm::SHA256);
    EXPECT_EQ(empty_salt.size(), out);
    EXPECT_EQ(empty_info.size(), out);
    EXPECT_NE(empty_salt, with_salt);
}

TEST_F(CryptoKdfAndTokensTest, HkdfRejectsInvalidDigestAndOversizedOutput) {
    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_THROW(qb::crypto::hkdf(test_input, test_salt, {}, 32, invalid_digest), std::runtime_error);

    // HKDF-SHA256 can emit at most 255 * HashLen octets.
    const auto sha256_limit = 255u * 32u;
    EXPECT_THROW(qb::crypto::hkdf(test_input, test_salt, {}, sha256_limit + 1u, qb::crypto::DigestAlgorithm::SHA256), std::runtime_error);

    EXPECT_TRUE(qb::crypto::hkdf(test_input, test_salt, {}, 0, qb::crypto::DigestAlgorithm::SHA256).empty());
}

// =============================================================================
// ARGON2 + unified derive_key
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, Argon2ReproducibleVariantsAndLength) {
#if defined(QB_HAS_ARGON2)
    // Reduced cost so the unit suite stays fast; the cryptographic property under
    // test is reproducibility, not work factor.
    qb::crypto::Argon2Params params;
    params.t_cost      = 1;
    params.m_cost      = 1 << 12; // 4 MiB
    params.parallelism = 1;

    // Implicit (random) salt -> two derivations of the same password differ.
    const auto random_a = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2id);
    const auto random_b = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2id);
    EXPECT_EQ(random_a.size(), 32u);
    EXPECT_NE(random_a, random_b);

    // Fixed salt -> the derivation is a byte-exact known-answer of itself (the
    // reproducibility KAT; a cross-implementation literal is not pinnable because
    // argon2_kdf hides the secret/associated-data inputs the RFC 9106 vectors use).
    params.salt          = "fixed_salt_for_test";
    const auto argon2d   = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2d);
    const auto argon2i   = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2i);
    const auto argon2id  = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2id);
    const auto argon2id2 = qb::crypto::argon2_kdf("password123", 32, params, qb::crypto::Argon2Variant::Argon2id);

    EXPECT_EQ(argon2d.size(), 32u);
    EXPECT_EQ(argon2i.size(), 32u);
    EXPECT_EQ(argon2id.size(), 32u);
    EXPECT_EQ(argon2id, argon2id2); // reproducible under a fixed salt
    EXPECT_NE(argon2d, argon2i);    // variants disagree
    EXPECT_NE(argon2d, argon2id);
    EXPECT_NE(argon2i, argon2id);

    EXPECT_THROW(qb::crypto::argon2_kdf("password123", 0, params, qb::crypto::Argon2Variant::Argon2id), std::runtime_error);
#else
    // Fallback (PBKDF2) path: fixed salt -> reproducible, password-sensitive.
    qb::crypto::Argon2Params params;
    params.salt = "fixed_salt_for_test";

    const auto key1 = qb::crypto::argon2_kdf("password123", 32, params);
    const auto key2 = qb::crypto::argon2_kdf("password123", 32, params);
    EXPECT_EQ(key1.size(), 32u);
    EXPECT_EQ(key1, key2);
    EXPECT_NE(key1, qb::crypto::argon2_kdf("different_password", 32, params));
#endif
}

TEST_F(CryptoKdfAndTokensTest, DeriveKeyHonoursLengthAndDistinguishesAlgorithms) {
    const auto salt = qb::crypto::generate_salt(16);

    const auto pbkdf2 = qb::crypto::derive_key("test_password", salt, 32, qb::crypto::KdfAlgorithm::PBKDF2, 10000);
    const auto hkdf   = qb::crypto::derive_key("test_password", salt, 32, qb::crypto::KdfAlgorithm::HKDF);
    const auto argon2 = qb::crypto::derive_key("test_password", salt, 32, qb::crypto::KdfAlgorithm::Argon2);

    EXPECT_EQ(pbkdf2.size(), 32u);
    EXPECT_EQ(hkdf.size(), 32u);
    EXPECT_EQ(argon2.size(), 32u);

    EXPECT_NE(pbkdf2, hkdf);
    EXPECT_NE(pbkdf2, argon2);
    EXPECT_NE(hkdf, argon2);
}

// =============================================================================
// BASE64URL
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, Base64UrlRoundTripsAndIsUrlSafe) {
    const std::string                input = "Hello, Base64URL!";
    const std::vector<unsigned char> input_vec(input.begin(), input.end());

    const std::string encoded = qb::crypto::base64url_encode(input_vec);
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
    EXPECT_EQ(encoded.find('='), std::string::npos);

    const auto decoded = qb::crypto::base64url_decode(encoded);
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), input);

    // RFC 4648 §10 base64url vectors + the URL-alphabet (-,_) byte pattern.
    const std::vector<std::pair<std::string, std::string>> vectors = {{"f", "Zg"}, {"fo", "Zm8"}, {"foo", "Zm9v"}};
    for (const auto &[plain, expected] : vectors) {
        const std::vector<unsigned char> bytes(plain.begin(), plain.end());
        EXPECT_EQ(qb::crypto::base64url_encode(bytes), expected);
        const auto round = qb::crypto::base64url_decode(expected);
        EXPECT_EQ(std::string(round.begin(), round.end()), plain);
    }

    const std::vector<unsigned char> url_bytes = {0xfb, 0xff};
    const std::string                url_encoded = qb::crypto::base64url_encode(url_bytes);
    EXPECT_EQ(url_encoded, "-_8");
    EXPECT_EQ(qb::crypto::base64url_decode(url_encoded), url_bytes);
    EXPECT_TRUE(qb::crypto::base64url_decode("").empty());
}

// =============================================================================
// SECURE TOKENS — round-trip, expiry (relative clock), malformed input
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, TokenRoundTripsAndRejectsWrongKey) {
    const std::string payload = "{\"user\":\"test\",\"admin\":false}";

    const std::string token = qb::crypto::generate_token(payload, test_key);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(qb::crypto::verify_token(token, test_key), payload);

    const auto wrong_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    EXPECT_TRUE(qb::crypto::verify_token(token, wrong_key).empty());
}

TEST_F(CryptoKdfAndTokensTest, TokenExpiryIsEnforcedWithoutSleeping) {
    const std::string payload = "{\"user\":\"test\"}";

    // A live token with a generous TTL verifies now.
    const std::string live = qb::crypto::generate_token(payload, test_key, std::chrono::seconds(3600));
    EXPECT_EQ(qb::crypto::verify_token(live, test_key), payload);

    // Manufacture an already-expired token by rewriting the embedded JSON `exp`
    // (epoch seconds) to the past, then re-sealing it with the same key — the same
    // AEAD envelope generate_token produces. No wall-clock wait, no flake.
    const auto iv = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    qb::json expired_json;
    expired_json["payload"] = payload;
    expired_json["exp"]     = unix_now_seconds() - 100; // 100s in the past

    const std::string              expired_str = expired_json.dump();
    const std::vector<unsigned char> expired_bytes(expired_str.begin(), expired_str.end());
    const auto                       ciphertext = qb::crypto::encrypt(expired_bytes, test_key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    std::vector<unsigned char> sealed;
    sealed.insert(sealed.end(), iv.begin(), iv.end());
    sealed.insert(sealed.end(), ciphertext.begin(), ciphertext.end());

    EXPECT_TRUE(qb::crypto::verify_token(qb::crypto::base64url_encode(sealed), test_key).empty());
}

TEST_F(CryptoKdfAndTokensTest, TokensRejectMalformedAndTruncatedInputs) {
    EXPECT_TRUE(qb::crypto::verify_token("", test_key).empty());
    EXPECT_TRUE(qb::crypto::verify_token("not-base64url", test_key).empty());

    // A blob shorter than a single AES-GCM IV cannot be a token.
    const std::vector<unsigned char> too_short(11, 0x41);
    EXPECT_TRUE(qb::crypto::verify_token(qb::crypto::base64url_encode(too_short), test_key).empty());

    // Authentic AEAD whose decrypted payload is not valid token JSON.
    const auto                       iv = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const std::string                invalid_json = "not-json";
    const std::vector<unsigned char> invalid_json_bytes(invalid_json.begin(), invalid_json.end());
    const auto encrypted_invalid = qb::crypto::encrypt(invalid_json_bytes, test_key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    std::vector<unsigned char> sealed;
    sealed.insert(sealed.end(), iv.begin(), iv.end());
    sealed.insert(sealed.end(), encrypted_invalid.begin(), encrypted_invalid.end());
    EXPECT_TRUE(qb::crypto::verify_token(qb::crypto::base64url_encode(sealed), test_key).empty());
}

TEST_F(CryptoKdfAndTokensTest, TokensCarryComplexPayloads) {
    const std::string json_payload = "{\"user_id\":123,\"roles\":[\"admin\",\"user\"],"
                                     "\"permissions\":{\"read\":true,\"write\":true}}";
    const std::string json_token = qb::crypto::generate_token(json_payload, test_key, std::chrono::seconds(60));
    EXPECT_EQ(qb::crypto::verify_token(json_token, test_key), json_payload);

    // Printable-ASCII payload (avoids invalid UTF-8 in the JSON envelope).
    std::string printable;
    printable.reserve(95);
    for (int c = 32; c < 127; ++c) {
        printable.push_back(static_cast<char>(c));
    }
    const std::string printable_token = qb::crypto::generate_token(printable, test_key);
    EXPECT_EQ(qb::crypto::verify_token(printable_token, test_key), printable);

    // Empty payload round-trips to empty.
    const std::string empty_token = qb::crypto::generate_token("", test_key);
    EXPECT_FALSE(empty_token.empty());
    EXPECT_EQ(qb::crypto::verify_token(empty_token, test_key), "");

    // 1 KiB payload.
    const std::string large_payload(1024, 'X');
    const std::string large_token = qb::crypto::generate_token(large_payload, test_key);
    EXPECT_EQ(qb::crypto::verify_token(large_token, test_key), large_payload);
}

// =============================================================================
// METADATA ENVELOPE
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, MetadataEnvelopeRoundTripsAndAuthenticates) {
    const std::vector<unsigned char> plaintext = {'s', 'e', 'c', 'r', 'e', 't'};
    const std::string                metadata  = "{\"user\":\"alice\",\"timestamp\":123456789}";

    const std::string encrypted = qb::crypto::encrypt_with_metadata(plaintext, test_key, metadata);
    EXPECT_FALSE(encrypted.empty());

    const auto result = qb::crypto::decrypt_with_metadata(encrypted, test_key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, plaintext);
    EXPECT_EQ(result->second, metadata);

    // Wrong key fails.
    const auto wrong_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata(encrypted, wrong_key).has_value());

    // Tampered (authenticated) metadata fails.
    std::string tampered = encrypted;
    if (const auto pos = tampered.find("alice"); pos != std::string::npos) {
        tampered.replace(pos, 5, "bobxx");
    }
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata(tampered, test_key).has_value());

    // Corrupted blob fails.
    std::string corrupted = encrypted;
    ASSERT_GT(corrupted.size(), 20u);
    corrupted[corrupted.size() / 2] ^= 0x01;
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata(corrupted, test_key).has_value());
}

TEST_F(CryptoKdfAndTokensTest, MetadataEnvelopeRejectsMalformedAndMismatchedAlgorithms) {
    const std::vector<unsigned char> plaintext = {'s', 'e', 'c', 'r', 'e', 't'};
    const std::string                metadata  = "{\"scope\":\"coverage\"}";

    const std::string encrypted = qb::crypto::encrypt_with_metadata(plaintext, test_key, metadata, qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    qb::json envelope = qb::json::parse(encrypted);
    envelope["alg"]   = static_cast<int>(qb::crypto::SymmetricAlgorithm::CHACHA20_POLY1305);

    EXPECT_FALSE(qb::crypto::decrypt_with_metadata(envelope.dump(), test_key, qb::crypto::SymmetricAlgorithm::AES_256_GCM).has_value());
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata("{not-json}", test_key).has_value());
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata("{}", test_key).has_value());

    const auto invalid_algorithm = static_cast<qb::crypto::SymmetricAlgorithm>(255);
    EXPECT_THROW(qb::crypto::encrypt_with_metadata(plaintext, test_key, metadata, invalid_algorithm), std::runtime_error);
}

TEST_F(CryptoKdfAndTokensTest, KeySerializationRoundTripsThroughMetadataEnvelope) {
    const auto original_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    const std::string metadata   = "{\"purpose\":\"test\",\"created\":\"2023-01-01\"}";
    const std::string serialized = qb::crypto::encrypt_with_metadata(original_key, test_key, metadata);
    EXPECT_FALSE(serialized.empty());

    const auto deserialized = qb::crypto::decrypt_with_metadata(serialized, test_key);
    ASSERT_TRUE(deserialized.has_value());
    EXPECT_EQ(deserialized->first, original_key);
    EXPECT_EQ(deserialized->second, metadata);

    const auto wrong_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    EXPECT_FALSE(qb::crypto::decrypt_with_metadata(serialized, wrong_key).has_value());
}

// =============================================================================
// PASSWORD HASHING
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, PasswordHashingVerifiesAndRejects) {
    const std::string password = "test_password";
    const std::string hash     = qb::crypto::hash_password(password);

    EXPECT_NE(hash, password);
#if defined(QB_HAS_ARGON2)
    EXPECT_EQ(hash.substr(0, 10), "$argon2id$");
#else
    EXPECT_EQ(hash.substr(0, 14), "$pbkdf2-sha256");
#endif

    EXPECT_TRUE(qb::crypto::verify_password(password, hash));
    EXPECT_FALSE(qb::crypto::verify_password("wrong_password", hash));
    EXPECT_FALSE(qb::crypto::verify_password(password, "invalid_hash_format"));

#if defined(QB_HAS_ARGON2)
    // Each Argon2 variant produces a distinct, still-verifiable hash. This pins the
    // bug history where verify_password() was variant-blind (only Argon2id verified).
    const std::string hash_d  = qb::crypto::hash_password(password, qb::crypto::Argon2Variant::Argon2d);
    const std::string hash_i  = qb::crypto::hash_password(password, qb::crypto::Argon2Variant::Argon2i);
    const std::string hash_id = qb::crypto::hash_password(password, qb::crypto::Argon2Variant::Argon2id);
    EXPECT_NE(hash_d, hash_i);
    EXPECT_NE(hash_d, hash_id);
    EXPECT_NE(hash_i, hash_id);
    EXPECT_TRUE(qb::crypto::verify_password(password, hash_d));
    EXPECT_TRUE(qb::crypto::verify_password(password, hash_i));
    EXPECT_TRUE(qb::crypto::verify_password(password, hash_id));
#endif

    // Empty, long and Unicode passwords all hash-and-verify.
    const std::string empty_hash = qb::crypto::hash_password("");
    EXPECT_TRUE(qb::crypto::verify_password("", empty_hash));
    EXPECT_FALSE(qb::crypto::verify_password("not_empty", empty_hash));

    const std::string long_password(1024, 'A');
    const std::string long_hash = qb::crypto::hash_password(long_password);
    EXPECT_TRUE(qb::crypto::verify_password(long_password, long_hash));
    EXPECT_FALSE(qb::crypto::verify_password(long_password + "X", long_hash));

    const std::string unicode_password = "\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c""123!@#"; // Cyrillic + specials
    const std::string unicode_hash     = qb::crypto::hash_password(unicode_password);
    EXPECT_TRUE(qb::crypto::verify_password(unicode_password, unicode_hash));

    // A second hash of the same password uses a fresh salt yet still verifies.
    const std::string hash2 = qb::crypto::hash_password(password);
    EXPECT_NE(hash, hash2);
    EXPECT_TRUE(qb::crypto::verify_password(password, hash2));
}

// =============================================================================
// UNIQUE IV
// =============================================================================

TEST_F(CryptoKdfAndTokensTest, UniqueIvIsUniqueAndSized) {
    std::vector<std::vector<unsigned char>> ivs;
    ivs.reserve(100);
    for (int i = 0; i < 100; ++i) {
        ivs.push_back(qb::crypto::generate_unique_iv(12));
    }

    for (const auto &iv : ivs) {
        EXPECT_EQ(iv.size(), 12u);
    }
    for (std::size_t i = 0; i < ivs.size(); ++i) {
        for (std::size_t j = i + 1; j < ivs.size(); ++j) {
            EXPECT_NE(ivs[i], ivs[j]);
        }
    }

    // Short and empty sizes are honoured exactly.
    EXPECT_TRUE(qb::crypto::generate_unique_iv(0).empty());
    EXPECT_EQ(qb::crypto::generate_unique_iv(4).size(), 4u);
    EXPECT_EQ(qb::crypto::generate_unique_iv(8).size(), 8u);
}

} // namespace
