/**
 * @file unit/crypto/crypto-primitives.cpp
 * @brief `qb::crypto` symmetric/digest/encoding primitives — pure logic, no engine.
 *
 * The crypto facade in qb/io/crypto.h is OpenSSL-backed but stateless: base64/hex
 * codecs, the MD5/SHA/BLAKE2 digest family (string + stream + iterated overloads),
 * HMAC, PBKDF2, AES-CBC/GCM + ChaCha20-Poly1305 AEAD, XOR, and the constant-time
 * comparator are all deterministic transforms over in-memory buffers. There is NO
 * event loop, NO socket, NO daemon — so this is a `unit` test (linking libcrypto is a
 * build dependency, not a runtime service). The contracts proven:
 *
 *   - base64 free-function + class API round-trip ASCII, binary (NUL/0xFF), and empty
 *     input exactly, and reject malformed alphabets;
 *   - hex codec is byte-exact for upper/lower ranges and rejects odd-length / non-hex;
 *   - digests match published known-answer vectors, including the empty-input digest of
 *     every primitive (MD5/SHA1/SHA256/SHA512) and iterated hashing;
 *   - stream overloads equal their string counterparts (untimed, single pass);
 *   - PBKDF2 matches the RFC 6070 (PBKDF2-HMAC-SHA1) known-answer vectors and rejects
 *     non-positive key sizes;
 *   - HMAC-SHA256 matches a known vector; the full DigestAlgorithm enum is size-pinned;
 *   - AES-128/192/256 CBC+GCM and ChaCha20-Poly1305 round-trip, reject wrong-size key/iv
 *     (typed throw), and fail AEAD authentication on tamper / wrong AAD / wrong key/iv;
 *   - constant_time_compare honours equal / differing / length-mismatch / empty.
 *
 * Folded from the dissolved system/test-crypto.cpp. The four legacy SKIP-MASKED tests
 * (Base64 / PBKDF2KeyDerivation / ErrorHandling / XOROperations — each `try/catch{cout}`
 * that swallowed failures and reported a pass either way) are collapsed here into hard
 * EXPECT_* / EXPECT_THROW assertions, the modern `Modern*`/`*Contracts` tests are kept as
 * the ground-truth supersets, the tautological `if (empty.size() == empty.size())` XOR
 * guard is dropped, and the file-local main() is removed for the shared gtest_main.
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

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>

namespace {

/**
 * @brief Shared fixture seeding a fixed plaintext used across the round-trip tests.
 */
class CryptoPrimitivesTest : public ::testing::Test {
protected:
    std::string                test_string;
    std::vector<unsigned char> test_data;

    void
    SetUp() override {
        test_string = "Hello, Crypto World!";
        test_data.assign(test_string.begin(), test_string.end());
    }
};

// =============================================================================
// BASE64 (folded from the legacy SKIP-MASKED `Base64` test)
// =============================================================================

TEST_F(CryptoPrimitivesTest, Base64RoundTripsAsciiBinaryAndEmpty) {
    const std::string expected = "SGVsbG8sIENyeXB0byBXb3JsZCE=";

    // Free-function API: byte-exact encode + round-trip.
    const std::string encoded = qb::crypto::base64_encode(test_data.data(), test_data.size());
    EXPECT_EQ(encoded, expected);
    EXPECT_EQ(qb::crypto::base64_decode(encoded), test_data);

    // Class API (was a `try/catch{cout}` swallow — now a hard expectation).
    const std::string class_encoded = qb::crypto::base64::encode(test_string);
    EXPECT_EQ(class_encoded, expected);
    EXPECT_EQ(qb::crypto::base64::decode(class_encoded), test_string);

    // Empty input must round-trip to empty (was swallowed).
    const std::vector<unsigned char> empty_data;
    const std::string                empty_encoded = qb::crypto::base64_encode(empty_data.data(), empty_data.size());
    EXPECT_TRUE(qb::crypto::base64_decode(empty_encoded).empty());

    // Binary payload with NUL and high bytes must round-trip exactly (was swallowed).
    const std::vector<unsigned char> binary_data    = {0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD};
    const std::string                binary_encoded = qb::crypto::base64_encode(binary_data.data(), binary_data.size());
    EXPECT_FALSE(binary_encoded.empty());
    EXPECT_EQ(qb::crypto::base64_decode(binary_encoded), binary_data);
}

TEST_F(CryptoPrimitivesTest, Base64ClassHandlesBinaryPayloadsAndRejectsGarbage) {
    const std::string binary{"\x00qb\x00crypto\xff", 11};

    const std::string encoded = qb::crypto::base64::encode(binary);
    EXPECT_FALSE(encoded.empty());
    EXPECT_EQ(qb::crypto::base64::decode(encoded), binary);

    const auto decoded_vector = qb::crypto::base64_decode(encoded);
    ASSERT_EQ(decoded_vector.size(), binary.size());
    EXPECT_EQ(std::string(decoded_vector.begin(), decoded_vector.end()), binary);

    EXPECT_TRUE(qb::crypto::base64::decode("%%%").empty());
}

// =============================================================================
// HEX CODEC
// =============================================================================

TEST_F(CryptoPrimitivesTest, HexEncodingAndParsingContracts) {
    const std::string binary{"Hello\x00\xff", 7};
    const std::string upper_hex = qb::crypto::to_hex_string(binary, qb::crypto::range_hex_upper);
    const std::string lower_hex = qb::crypto::to_hex_string(binary, qb::crypto::range_hex_lower);

    EXPECT_EQ(upper_hex, "48656C6C6F00FF");
    EXPECT_EQ(lower_hex, "48656c6c6f00ff");
    EXPECT_EQ(qb::crypto::hex_to_string(upper_hex), binary);
    EXPECT_EQ(qb::crypto::hex_to_string(lower_hex), binary);

    EXPECT_EQ(qb::crypto::hex_value('0'), 0);
    EXPECT_EQ(qb::crypto::hex_value('9'), 9);
    EXPECT_EQ(qb::crypto::hex_value('A'), 10);
    EXPECT_EQ(qb::crypto::hex_value('F'), 15);
    EXPECT_EQ(qb::crypto::hex_value('a'), 10);
    EXPECT_EQ(qb::crypto::hex_value('f'), 15);
    EXPECT_EQ(qb::crypto::hex_value('g'), -1);
    EXPECT_EQ(qb::crypto::hex_value(':'), -1);

    // Odd length and non-hex input must decode to empty, not garbage.
    EXPECT_TRUE(qb::crypto::hex_to_string("F").empty());
    EXPECT_TRUE(qb::crypto::hex_to_string("GG").empty());
    EXPECT_TRUE(qb::crypto::hex_to_string("00xz").empty());
}

// =============================================================================
// DIGESTS — known-answer vectors incl. empty input
// =============================================================================

TEST_F(CryptoPrimitivesTest, DigestKnownAnswerVectors) {
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::md5(test_string), qb::crypto::range_hex_lower), "39076285a6c5ba8ecb12881f3263987f");
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::sha1(test_string), qb::crypto::range_hex_lower),
              "93fcd83c3e94fd6b028c811033333c42e9c5cc6b");
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::sha256(test_string), qb::crypto::range_hex_lower),
              "9a15e201db8dbc4fe4ad851cc66e28c650400393ee05932d22132cfae71c803b");
    EXPECT_EQ(
        qb::crypto::to_hex_string(qb::crypto::sha512(test_string), qb::crypto::range_hex_lower),
        "13365f2c51fb536130b1cdb2da3b89968a4dbe45fc14ec786d47f0b9345faace1c1b45f23ef6ba71b74016cc300c31c9a5412201db29e3cd7f0ab175664986ab");
}

TEST_F(CryptoPrimitivesTest, EmptyInputDigestVectors) {
    // The canonical empty-string digests — a broken codec that mishandles the
    // zero-length path is caught here rather than only on non-empty input.
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::md5(""), qb::crypto::range_hex_lower), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::sha1(""), qb::crypto::range_hex_lower), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(qb::crypto::to_hex_string(qb::crypto::sha256(""), qb::crypto::range_hex_lower),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(
        qb::crypto::to_hex_string(qb::crypto::sha512(""), qb::crypto::range_hex_lower),
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

TEST_F(CryptoPrimitivesTest, IteratedHashingDiffersFromSinglePass) {
    EXPECT_NE(qb::crypto::md5(test_string, 3), qb::crypto::md5(test_string));
    EXPECT_NE(qb::crypto::sha1(test_string, 3), qb::crypto::sha1(test_string));
    EXPECT_NE(qb::crypto::sha256(test_string, 3), qb::crypto::sha256(test_string));
    EXPECT_NE(qb::crypto::sha512(test_string, 3), qb::crypto::sha512(test_string));
}

TEST_F(CryptoPrimitivesTest, StreamHashesMatchStringHashes) {
    std::istringstream md5_stream(test_string);
    std::istringstream sha1_stream(test_string);
    std::istringstream sha256_stream(test_string);
    std::istringstream sha512_stream(test_string);

    EXPECT_EQ(qb::crypto::md5(md5_stream), qb::crypto::md5(test_string));
    EXPECT_EQ(qb::crypto::sha1(sha1_stream), qb::crypto::sha1(test_string));
    EXPECT_EQ(qb::crypto::sha256(sha256_stream), qb::crypto::sha256(test_string));
    EXPECT_EQ(qb::crypto::sha512(sha512_stream), qb::crypto::sha512(test_string));
}

TEST_F(CryptoPrimitivesTest, IteratedStreamHashesMatchIteratedStringHashes) {
    std::istringstream md5_stream(test_string);
    std::istringstream sha1_stream(test_string);
    std::istringstream sha256_stream(test_string);
    std::istringstream sha512_stream(test_string);

    EXPECT_EQ(qb::crypto::md5(md5_stream, 3), qb::crypto::md5(test_string, 3));
    EXPECT_EQ(qb::crypto::sha1(sha1_stream, 3), qb::crypto::sha1(test_string, 3));
    EXPECT_EQ(qb::crypto::sha256(sha256_stream, 3), qb::crypto::sha256(test_string, 3));
    EXPECT_EQ(qb::crypto::sha512(sha512_stream, 3), qb::crypto::sha512(test_string, 3));
}

TEST_F(CryptoPrimitivesTest, ModernDigestAndHmacAlgorithms) {
    struct DigestCase {
        qb::crypto::DigestAlgorithm algorithm;
        std::size_t                 expected_size;
    };

    const std::vector<DigestCase> cases = {
        {qb::crypto::DigestAlgorithm::MD5, 16},        {qb::crypto::DigestAlgorithm::SHA1, 20},       {qb::crypto::DigestAlgorithm::SHA224, 28},
        {qb::crypto::DigestAlgorithm::SHA256, 32},     {qb::crypto::DigestAlgorithm::SHA384, 48},     {qb::crypto::DigestAlgorithm::SHA512, 64},
        {qb::crypto::DigestAlgorithm::BLAKE2B512, 64}, {qb::crypto::DigestAlgorithm::BLAKE2S256, 32},
    };

    const std::vector<unsigned char> hmac_key = {'s', 'e', 'c', 'r', 'e', 't'};

    for (const auto &entry : cases) {
        ASSERT_NE(qb::crypto::get_evp_md(entry.algorithm), nullptr);

        const auto digest = qb::crypto::hash(test_data, entry.algorithm);
        EXPECT_EQ(digest.size(), entry.expected_size);

        const auto mac = qb::crypto::hmac(test_data, hmac_key, entry.algorithm);
        EXPECT_EQ(mac.size(), entry.expected_size);
    }

    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_EQ(qb::crypto::get_evp_md(invalid_digest), nullptr);
    EXPECT_THROW(qb::crypto::hash(test_data, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::hmac(test_data, hmac_key, invalid_digest), std::runtime_error);
}

TEST_F(CryptoPrimitivesTest, VectorSha256OverloadMatchesStringDigest) {
    // The std::vector<unsigned char> -> std::vector<unsigned char> SHA256 overload is a
    // distinct entry point from the string/stream `sha256` family: it returns the raw
    // 32-byte digest directly (no hex encoding, no iteration). Pin it against the same
    // known-answer vector the string overload is checked against in
    // DigestKnownAnswerVectors so a divergence in the two code paths is caught.
    const auto raw = qb::crypto::sha256(test_data);
    ASSERT_EQ(raw.size(), static_cast<std::size_t>(32));
    EXPECT_EQ(qb::crypto::to_hex_string(std::string(raw.begin(), raw.end()), qb::crypto::range_hex_lower),
              "9a15e201db8dbc4fe4ad851cc66e28c650400393ee05932d22132cfae71c803b");

    // Empty input yields the canonical empty-string SHA256.
    const auto empty_raw = qb::crypto::sha256(std::vector<unsigned char>{});
    ASSERT_EQ(empty_raw.size(), static_cast<std::size_t>(32));
    EXPECT_EQ(qb::crypto::to_hex_string(std::string(empty_raw.begin(), empty_raw.end()), qb::crypto::range_hex_lower),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // The raw-vector digest must equal the string overload for the same input. Note the
    // string `sha256` returns the RAW 32-byte digest (it resizes to SHA256_DIGEST_LENGTH
    // and writes the digest directly — no hex encoding), so the two overloads agree
    // byte-for-byte with no decoding step.
    const std::string string_form = qb::crypto::sha256(test_string);
    EXPECT_EQ(std::string(raw.begin(), raw.end()), string_form);
}

TEST_F(CryptoPrimitivesTest, HmacSha256MatchesKnownVector) {
    const std::vector<unsigned char> key  = {'k', 'e', 'y'};
    const std::string                data = "The quick brown fox jumps over the lazy dog";

    const auto mac = qb::crypto::hmac_sha256(key, data);
    EXPECT_EQ(qb::crypto::to_hex_string(std::string(mac.begin(), mac.end()), qb::crypto::range_hex_lower),
              "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

// =============================================================================
// PBKDF2 — RFC 6070 known-answer vectors (folded from the SKIP-MASKED legacy test)
// =============================================================================

TEST_F(CryptoPrimitivesTest, Pbkdf2MatchesRfc6070Vectors) {
    // qb::crypto::pbkdf2 uses PKCS5_PBKDF2_HMAC_SHA1, so the RFC 6070
    // PBKDF2-HMAC-SHA1 test vectors apply byte-for-byte.
    const auto hex = [](const std::string &raw) {
        return qb::crypto::to_hex_string(raw, qb::crypto::range_hex_lower);
    };

    EXPECT_EQ(hex(qb::crypto::pbkdf2("password", "salt", 1, 20)), "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    EXPECT_EQ(hex(qb::crypto::pbkdf2("password", "salt", 2, 20)), "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    EXPECT_EQ(hex(qb::crypto::pbkdf2("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 25)),
              "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
}

TEST_F(CryptoPrimitivesTest, Pbkdf2IsDeterministicAndParameterSensitive) {
    const std::string password = "secure_password";
    const std::string salt     = "random_salt";

    const std::string key1 = qb::crypto::pbkdf2(password, salt, 1000, 16);
    const std::string key2 = qb::crypto::pbkdf2(password, salt, 1000, 32);
    const std::string key3 = qb::crypto::pbkdf2(password, salt, 2000, 16);

    EXPECT_EQ(key1.size(), 16u);
    EXPECT_EQ(key2.size(), 32u);
    EXPECT_EQ(key3.size(), 16u);

    EXPECT_NE(key1, key2);                                                     // key size matters
    EXPECT_NE(key1, key3);                                                     // iteration count matters
    EXPECT_EQ(key1, qb::crypto::pbkdf2(password, salt, 1000, 16));             // deterministic
    EXPECT_NE(key1, qb::crypto::pbkdf2("different_password", salt, 1000, 16)); // password matters
    EXPECT_NE(key1, qb::crypto::pbkdf2(password, "different_salt", 1000, 16)); // salt matters

    // Empty password and empty salt are accepted and produce the requested length
    // (was a `try/catch{cout}` swallow — now a hard expectation).
    EXPECT_EQ(qb::crypto::pbkdf2("", salt, 1000, 16).size(), 16u);
    EXPECT_EQ(qb::crypto::pbkdf2(password, "", 1000, 16).size(), 16u);

    // A single iteration is permitted (was swallowed).
    EXPECT_EQ(qb::crypto::pbkdf2(password, salt, 1, 16).size(), 16u);
}

TEST_F(CryptoPrimitivesTest, Pbkdf2RejectsInvalidKeySizes) {
    EXPECT_TRUE(qb::crypto::pbkdf2("password", "salt", 1000, 0).empty());
    EXPECT_TRUE(qb::crypto::pbkdf2("password", "salt", 1000, -1).empty());

    const std::string key = qb::crypto::pbkdf2("password", "salt", 1, 8);
    ASSERT_EQ(key.size(), 8u);
    EXPECT_EQ(key, qb::crypto::pbkdf2("password", "salt", 1, 8));
}

// =============================================================================
// SYMMETRIC ENCRYPTION — round-trip + AEAD authentication contracts
// =============================================================================

TEST_F(CryptoPrimitivesTest, ModernSymmetricAlgorithmsRoundTripAndRejectBadInputs) {
    struct AlgorithmCase {
        qb::crypto::SymmetricAlgorithm algorithm;
        std::size_t                    expected_key_size;
        std::size_t                    expected_iv_size;
        bool                           aead;
    };

    const std::vector<AlgorithmCase> cases = {
        {qb::crypto::SymmetricAlgorithm::AES_128_CBC, 16, 16, false},      {qb::crypto::SymmetricAlgorithm::AES_192_CBC, 24, 16, false},
        {qb::crypto::SymmetricAlgorithm::AES_256_CBC, 32, 16, false},      {qb::crypto::SymmetricAlgorithm::AES_128_GCM, 16, 12, true},
        {qb::crypto::SymmetricAlgorithm::AES_192_GCM, 24, 12, true},       {qb::crypto::SymmetricAlgorithm::AES_256_GCM, 32, 12, true},
        {qb::crypto::SymmetricAlgorithm::CHACHA20_POLY1305, 32, 12, true},
    };

    const std::vector<unsigned char> aad = {'q', 'b', '-', 'a', 'a', 'd'};

    for (const auto &entry : cases) {
        auto key = qb::crypto::generate_key(entry.algorithm);
        auto iv  = qb::crypto::generate_iv(entry.algorithm);

        EXPECT_EQ(key.size(), entry.expected_key_size);
        EXPECT_EQ(iv.size(), entry.expected_iv_size);

        const auto encrypted = qb::crypto::encrypt(test_data, key, iv, entry.algorithm, aad);
        ASSERT_FALSE(encrypted.empty());

        const auto decrypted = qb::crypto::decrypt(encrypted, key, iv, entry.algorithm, aad);
        EXPECT_EQ(decrypted, test_data);

        // Empty plaintext round-trips to empty.
        const auto encrypted_empty = qb::crypto::encrypt({}, key, iv, entry.algorithm, aad);
        EXPECT_TRUE(qb::crypto::decrypt(encrypted_empty, key, iv, entry.algorithm, aad).empty());

        // Wrong-size key / iv are typed errors.
        auto bad_key = key;
        bad_key.pop_back();
        EXPECT_THROW(qb::crypto::encrypt(test_data, bad_key, iv, entry.algorithm), std::invalid_argument);
        EXPECT_THROW(qb::crypto::decrypt(encrypted, bad_key, iv, entry.algorithm), std::invalid_argument);

        auto bad_iv = iv;
        bad_iv.pop_back();
        EXPECT_THROW(qb::crypto::encrypt(test_data, key, bad_iv, entry.algorithm), std::invalid_argument);
        EXPECT_THROW(qb::crypto::decrypt(encrypted, key, bad_iv, entry.algorithm), std::invalid_argument);

        if (entry.aead) {
            // Tamper, wrong AAD, wrong key, and wrong IV all fail authentication
            // and return empty (folded from the legacy SKIP-MASKED ErrorHandling).
            auto tampered = encrypted;
            tampered.back() ^= 0x01;
            EXPECT_TRUE(qb::crypto::decrypt(tampered, key, iv, entry.algorithm, aad).empty());

            const std::vector<unsigned char> wrong_aad = {'w', 'r', 'o', 'n', 'g'};
            EXPECT_TRUE(qb::crypto::decrypt(encrypted, key, iv, entry.algorithm, wrong_aad).empty());

            const auto wrong_key = qb::crypto::generate_key(entry.algorithm);
            EXPECT_TRUE(qb::crypto::decrypt(encrypted, wrong_key, iv, entry.algorithm, aad).empty());

            const auto wrong_iv = qb::crypto::generate_iv(entry.algorithm);
            EXPECT_TRUE(qb::crypto::decrypt(encrypted, key, wrong_iv, entry.algorithm, aad).empty());

            std::vector<unsigned char> too_short(8, 0x42);
            EXPECT_THROW(qb::crypto::decrypt(too_short, key, iv, entry.algorithm), std::runtime_error);

            // An OVERSIZED AEAD nonce must be rejected, not silently truncated to
            // 12 bytes: the cipher is initialised with the default length and
            // never SET_IVLEN, so accepting a longer IV let a caller varying only
            // its tail reuse the same 96-bit GCM nonce (key/tag recovery). The IV
            // length is now required to be exact for AEAD too.
            auto oversized_iv = iv;
            oversized_iv.push_back(0x00);
            oversized_iv.push_back(0x00); // 14 bytes for a 12-byte nonce cipher
            EXPECT_THROW(qb::crypto::encrypt(test_data, key, oversized_iv, entry.algorithm, aad), std::invalid_argument);
            EXPECT_THROW(qb::crypto::decrypt(encrypted, key, oversized_iv, entry.algorithm, aad), std::invalid_argument);
        }
    }

    const auto invalid_algorithm = static_cast<qb::crypto::SymmetricAlgorithm>(255);
    EXPECT_THROW(qb::crypto::generate_key(invalid_algorithm), std::runtime_error);
    EXPECT_THROW(qb::crypto::generate_iv(invalid_algorithm), std::runtime_error);
    EXPECT_THROW(qb::crypto::encrypt(test_data, {}, {}, invalid_algorithm), std::runtime_error);
    EXPECT_THROW(qb::crypto::decrypt(test_data, {}, {}, invalid_algorithm), std::runtime_error);
}

TEST_F(CryptoPrimitivesTest, AesGcmDecryptRejectsWrongAlgorithm) {
    const auto key       = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const auto iv        = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const auto encrypted = qb::crypto::encrypt(test_data, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    ASSERT_FALSE(encrypted.empty());

    // Decrypting AES-256-GCM ciphertext as AES-256-CBC must not yield the plaintext.
    // The CBC path may either return empty/garbage or throw on a padding error — both
    // are acceptable failure modes; the only forbidden outcome is recovering the data.
    std::vector<unsigned char> decrypted;
    try {
        decrypted = qb::crypto::decrypt(encrypted, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_CBC);
    } catch (const std::exception &) {
        decrypted.clear();
    }
    EXPECT_NE(decrypted, test_data);
}

TEST_F(CryptoPrimitivesTest, CbcDecryptThrowsOnFinalizationFailure) {
    // The non-AEAD (CBC) decrypt path has a distinct failure mode from the AEAD modes:
    // an authentication/padding failure on a CBC cipher does NOT return empty (that is
    // the AEAD contract), it THROWS std::runtime_error from the EVP_DecryptFinal_ex
    // branch. A ciphertext whose length is not a multiple of the AES block size (16)
    // deterministically fails finalization with a "wrong final block length" error,
    // exercising that throw without relying on probabilistic padding corruption.
    const auto key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_CBC);
    const auto iv  = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_CBC);

    // 17 bytes: passes validate_symmetric_parameters (key/iv sizes are correct) but is
    // not block-aligned, so DecryptFinal rejects it.
    const std::vector<unsigned char> not_block_aligned(17, 0x5A);
    EXPECT_THROW(qb::crypto::decrypt(not_block_aligned, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_CBC), std::runtime_error);

    // A genuinely block-aligned but cryptographically meaningless block also fails
    // finalization for CBC (invalid PKCS7 padding) and must throw, never return data.
    auto valid = qb::crypto::encrypt(test_data, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_CBC);
    ASSERT_FALSE(valid.empty());
    ASSERT_EQ(valid.size() % 16u, 0u);
    // Flip every byte of the final block so the recovered padding is overwhelmingly
    // invalid; on the rare chance it decodes to valid padding the plaintext still
    // cannot equal the original, which the EXPECT_NE below would catch — but the
    // dominant, asserted outcome here is the throw.
    for (std::size_t i = valid.size() - 16u; i < valid.size(); ++i) {
        valid[i] ^= 0xFF;
    }
    std::vector<unsigned char> recovered;
    bool                       threw = false;
    try {
        recovered = qb::crypto::decrypt(valid, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_CBC);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    EXPECT_TRUE(threw || recovered != test_data);
}

// =============================================================================
// XOR (folded from the legacy SKIP-MASKED `XOROperations`, tautology dropped)
// =============================================================================

TEST_F(CryptoPrimitivesTest, XorBytesContracts) {
    const std::vector<unsigned char> a        = {0x01, 0x02, 0x03, 0x04, 0x05};
    const std::vector<unsigned char> b        = {0x10, 0x20, 0x30, 0x40, 0x50};
    const std::vector<unsigned char> expected = {0x11, 0x22, 0x33, 0x44, 0x55};

    EXPECT_EQ(qb::crypto::xor_bytes(a, b), expected);

    // Self-XOR is all-zero.
    EXPECT_EQ(qb::crypto::xor_bytes(a, a), std::vector<unsigned char>(a.size(), 0));

    // Empty-vs-empty is empty (was swallowed in a `try/catch{cout}`).
    EXPECT_TRUE(qb::crypto::xor_bytes({}, {}).empty());

    // Unequal lengths are a hard error: pin the actual contract rather than
    // accepting either truncation or throw (the legacy test allowed both).
    const std::vector<unsigned char> shorter = {0x01, 0x02, 0x03};
    const std::vector<unsigned char> longer  = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    EXPECT_THROW(qb::crypto::xor_bytes(shorter, longer), std::runtime_error);
    EXPECT_THROW(qb::crypto::xor_bytes(longer, shorter), std::runtime_error);
}

// =============================================================================
// RANDOM + CONSTANT-TIME COMPARE
// =============================================================================

TEST_F(CryptoPrimitivesTest, SecureRandomStringValidatesRangeAndLength) {
    EXPECT_EQ(qb::crypto::generate_secure_random_string(0), "");
    EXPECT_THROW(qb::crypto::generate_secure_random_string(1, ""), std::invalid_argument);

    const std::string too_large_range(257, 'x');
    EXPECT_THROW(qb::crypto::generate_secure_random_string(1, too_large_range), std::invalid_argument);

    const std::string random = qb::crypto::generate_secure_random_string(64, "ab");
    ASSERT_EQ(random.size(), 64u);
    for (const char c : random) {
        EXPECT_TRUE(c == 'a' || c == 'b');
    }

    // Two independent draws must differ (catches a stuck CSPRNG).
    EXPECT_NE(qb::crypto::generate_secure_random_string(64), qb::crypto::generate_secure_random_string(64));
}

TEST_F(CryptoPrimitivesTest, RandomGeneratorFactoryIsSeededAndUsable) {
    // crypto::random_generator<T>() builds a std::seed_seq from std::random_device and
    // returns a freshly-seeded engine of the requested type. Exercise the default
    // (mt19937) and an explicit 64-bit instantiation, proving each produces a working,
    // non-stuck generator and that two independent factory calls do not return the
    // same seeded state (the seed-from-random_device path actually ran).
    auto gen32_a = qb::crypto::random_generator<>();
    auto gen32_b = qb::crypto::random_generator<>();

    std::vector<std::mt19937::result_type> seq_a;
    std::vector<std::mt19937::result_type> seq_b;
    seq_a.reserve(8);
    seq_b.reserve(8);
    bool varies = false;
    auto first  = gen32_a();
    for (int i = 0; i < 8; ++i) {
        const auto v = gen32_a();
        seq_a.push_back(v);
        seq_b.push_back(gen32_b());
        if (v != first) {
            varies = true;
        }
    }
    // A working PRNG must not emit a constant stream...
    EXPECT_TRUE(varies);
    // ...and two independently seeded engines must diverge (catches an unseeded /
    // constant-seed factory).
    EXPECT_NE(seq_a, seq_b);

    // Explicit 64-bit engine instantiation compiles and produces a varying stream too.
    auto       gen64    = qb::crypto::random_generator<std::mt19937_64>();
    const auto v0       = gen64();
    bool       varies64 = false;
    for (int i = 0; i < 8; ++i) {
        if (gen64() != v0) {
            varies64 = true;
        }
    }
    EXPECT_TRUE(varies64);
}

TEST_F(CryptoPrimitivesTest, ConstantTimeCompareContracts) {
    const std::vector<unsigned char> a = {1, 2, 3, 4};
    const std::vector<unsigned char> b = {1, 2, 3, 4};
    const std::vector<unsigned char> c = {1, 2, 3, 5};
    const std::vector<unsigned char> d = {1, 2, 3};

    EXPECT_TRUE(qb::crypto::constant_time_compare(a, b));
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, c));
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, d));
    EXPECT_TRUE(qb::crypto::constant_time_compare({}, {}));
}

// =============================================================================
// hmac() error paths must not double-free (regression: heap corruption)
// =============================================================================

/**
 * @test An HMAC key that OpenSSL rejects fails cleanly instead of double-freeing
 * @brief Regression (DOUBLE FREE / heap corruption, found by fuzzing): `crypto::hmac` owned its
 *        `EVP_MD_CTX` in the enclosing `catch`, yet EVERY error path inside the `try` also did
 *        `EVP_MD_CTX_free(mdctx); throw;` — so the catch freed it a SECOND time. Its three sibling
 *        functions (`encrypt`, `decrypt`, `hash`) all get this right; `hmac` was the lone outlier.
 *
 *        The trigger needs no attacker: an EMPTY key. `EVP_PKEY_new_mac_key(..., nullptr, 0)`
 *        fails, the first error path fires, and the process double-frees — so a service whose JWT
 *        secret comes from an unset environment variable corrupts its heap on the very first
 *        `jwt::create()` / `jwt::verify()`. Under ASan the pre-fix code ABORTS here rather than
 *        failing the assertion, which is the negative proof.
 */
TEST(Crypto, HmacWithRejectedKeyThrowsWithoutDoubleFree) {
    const std::vector<unsigned char> data{'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    const std::vector<unsigned char> empty_key;

    for (const auto alg : {qb::crypto::DigestAlgorithm::SHA256, qb::crypto::DigestAlgorithm::SHA512,
                           qb::crypto::DigestAlgorithm::SHA1}) {
        EXPECT_THROW((void) qb::crypto::hmac(data, empty_key, alg), std::runtime_error)
            << "an empty HMAC key must be rejected, not silently accepted";
    }

    // The library is still usable afterwards — the failed call left no corrupted state.
    const std::vector<unsigned char> good_key{'k', 'e', 'y', '-', 'm', 'a', 't', 'e', 'r', 'i', 'a', 'l'};
    const auto                       mac = qb::crypto::hmac(data, good_key, qb::crypto::DigestAlgorithm::SHA256);
    EXPECT_EQ(mac.size(), 32u);
    EXPECT_EQ(qb::crypto::hmac(data, good_key, qb::crypto::DigestAlgorithm::SHA256), mac) << "HMAC must be deterministic";
}

} // namespace
