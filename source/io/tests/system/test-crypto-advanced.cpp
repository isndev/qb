/**
 * @file qb/source/io/tests/system/test-crypto-advanced.cpp
 * @brief Tests for advanced cryptographic functions in the qb IO library
 * 
 * This file contains unit tests for HKDF, Argon2, secure tokens, and other
 * advanced cryptographic functionality.
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
 * @ingroup IO
 */

#include <gtest/gtest.h>
#include <qb/io/crypto.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

namespace {

// Test fixture for advanced cryptographic functions
class CryptoAdvancedTest : public ::testing::Test {
protected:
    // Test data
    std::vector<unsigned char> test_input;
    std::vector<unsigned char> test_salt;
    std::vector<unsigned char> test_key;
    
    void SetUp() override {
        // Initialize test data
        test_input = {'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
        test_salt = qb::crypto::generate_salt(16);
        test_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    }
};

// Tests for HKDF
TEST_F(CryptoAdvancedTest, HKDF) {
    // Test vectors from RFC 5869
    const char* ikm_hex = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b";
    const char* salt_hex = "000102030405060708090a0b0c";
    const char* info_hex = "f0f1f2f3f4f5f6f7f8f9";
    const char* expected_okm_hex = "3cb25f25faacd57a90434f64d0362f2a"
                                   "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                                   "34007208d5b887185865";
    
    // Convert hex to binary
    auto hex_to_bytes = [](const std::string& hex) {
        std::vector<unsigned char> bytes;
        for (size_t i = 0; i < hex.length(); i += 2) {
            std::string byteString = hex.substr(i, 2);
            bytes.push_back(static_cast<unsigned char>(std::stoi(byteString, nullptr, 16)));
        }
        return bytes;
    };
    
    std::vector<unsigned char> ikm = hex_to_bytes(ikm_hex);
    std::vector<unsigned char> salt = hex_to_bytes(salt_hex);
    std::vector<unsigned char> info = hex_to_bytes(info_hex);
    std::vector<unsigned char> expected_okm = hex_to_bytes(expected_okm_hex);
    
    // Test HKDF
    std::vector<unsigned char> okm = qb::crypto::hkdf(
        ikm, salt, info, expected_okm.size(), qb::crypto::DigestAlgorithm::SHA256);
    
    // Check result
    EXPECT_EQ(okm, expected_okm);
    
    // Test with empty info
    std::vector<unsigned char> okm2 = qb::crypto::hkdf(
        test_input, test_salt, {}, 32, qb::crypto::DigestAlgorithm::SHA256);
    
    // Just check size
    EXPECT_EQ(okm2.size(), 32);
    
    // Test with different digest algorithms
    std::vector<unsigned char> okm3 = qb::crypto::hkdf(
        test_input, test_salt, {}, 32, qb::crypto::DigestAlgorithm::SHA512);
    
    EXPECT_EQ(okm3.size(), 32);
    EXPECT_NE(okm2, okm3); // Different digest should give different output
}

// Tests for Argon2 key derivation
TEST_F(CryptoAdvancedTest, Argon2KeyDerivation) {
    // Test Argon2id variant (most commonly used)
    qb::crypto::Argon2Variant variant = qb::crypto::Argon2Variant::Argon2id;
    
    // Create params with small values for faster testing
    qb::crypto::Argon2Params params;
    params.t_cost = 1;        // 1 iteration
    params.m_cost = 1 << 12;  // 4 MiB
    params.parallelism = 1;   // 1 thread
    
    // Generate key
    std::vector<unsigned char> key1 = qb::crypto::argon2_kdf(
        "password", 32, params, variant);
    
    // Should have requested length
    EXPECT_EQ(key1.size(), 32);
    
    // Generate key with different password
    std::vector<unsigned char> key2 = qb::crypto::argon2_kdf(
        "different", 32, params, variant);
    
    // Should be different
    EXPECT_NE(key1, key2);
}

// Tests for high-level key derivation
TEST_F(CryptoAdvancedTest, KeyDerivation) {
    // Test all KDF algorithms
    std::vector<qb::crypto::KdfAlgorithm> algorithms = {
        qb::crypto::KdfAlgorithm::PBKDF2,
        qb::crypto::KdfAlgorithm::HKDF,
        qb::crypto::KdfAlgorithm::Argon2
    };
    
    for (auto algorithm : algorithms) {
        // Derive key
        std::vector<unsigned char> key = qb::crypto::derive_key(
            "password", test_salt, 32, algorithm);
        
        // Should have requested length
        EXPECT_EQ(key.size(), 32);
        
        // Derive key with different password
        std::vector<unsigned char> key2 = qb::crypto::derive_key(
            "different", test_salt, 32, algorithm);
        
        // Should be different
        EXPECT_NE(key, key2);
    }
}

// Tests for constant-time comparison
TEST_F(CryptoAdvancedTest, ConstantTimeCompare) {
    // Test equal values
    std::vector<unsigned char> a = {1, 2, 3, 4, 5};
    std::vector<unsigned char> b = {1, 2, 3, 4, 5};
    
    EXPECT_TRUE(qb::crypto::constant_time_compare(a, b));
    
    // Test different values
    std::vector<unsigned char> c = {1, 2, 3, 4, 6};
    
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, c));
    
    // Test different lengths
    std::vector<unsigned char> d = {1, 2, 3, 4};
    
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, d));
    
    // Test empty values
    std::vector<unsigned char> e;
    std::vector<unsigned char> f;
    
    EXPECT_TRUE(qb::crypto::constant_time_compare(e, f));
    EXPECT_FALSE(qb::crypto::constant_time_compare(a, e));
}

// Tests for Base64URL encoding/decoding
TEST_F(CryptoAdvancedTest, DISABLED_Base64URL) {
    // Test vectors
    std::vector<std::pair<std::vector<unsigned char>, std::string>> test_vectors = {
        {{}, ""},
        {{'f'}, "Zg"},
        {{'f', 'o'}, "Zm8"},
        {{'f', 'o', 'o'}, "Zm9v"},
        {{'f', 'o', 'o', 'b'}, "Zm9vYg"},
        {{'f', 'o', 'o', 'b', 'a'}, "Zm9vYmE"},
        {{'f', 'o', 'o', 'b', 'a', 'r'}, "Zm9vYmFy"}
    };
    
    for (const auto& test : test_vectors) {
        // Encode
        std::string encoded = qb::crypto::base64url_encode(test.first);
        
        // Check result
        EXPECT_EQ(encoded, test.second);
        
        // Decode
        std::vector<unsigned char> decoded = qb::crypto::base64url_decode(encoded);
        
        // Check result
        EXPECT_EQ(decoded, test.first);
    }
    
    // Test URL safety - should not contain '+', '/', or '='
    std::vector<unsigned char> random_data = qb::crypto::generate_random_bytes(64);
    std::string encoded = qb::crypto::base64url_encode(random_data);
    
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
    EXPECT_EQ(encoded.find('='), std::string::npos);
}

// Tests for token generation and verification
TEST_F(CryptoAdvancedTest, Tokens) {
    // Generate token
    std::string payload = "{\"user\":\"test\",\"admin\":false}";
    std::string token = qb::crypto::generate_token(payload, test_key);
    
    // Token should not be empty
    EXPECT_FALSE(token.empty());
    
    // Verify token
    std::string verified_payload = qb::crypto::verify_token(token, test_key);
    
    // Should get original payload
    EXPECT_EQ(verified_payload, payload);
    
    // Test with TTL
    std::string token_with_ttl = qb::crypto::generate_token(payload, test_key, 1);
    
    // Token should not be empty
    EXPECT_FALSE(token_with_ttl.empty());
    
    // Verify token immediately (should work)
    verified_payload = qb::crypto::verify_token(token_with_ttl, test_key);
    EXPECT_EQ(verified_payload, payload);
    
    // Wait for token to expire
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Verify token after expiration (should fail)
    verified_payload = qb::crypto::verify_token(token_with_ttl, test_key);
    EXPECT_TRUE(verified_payload.empty());
    
    // Test with wrong key
    std::vector<unsigned char> wrong_key = qb::crypto::generate_key(
        qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    verified_payload = qb::crypto::verify_token(token, wrong_key);
    EXPECT_TRUE(verified_payload.empty());
}

// Tests for password hashing
TEST_F(CryptoAdvancedTest, DISABLED_PasswordHashing) {
    // Hash a password
    std::string password = "test_password";
    std::string hash = qb::crypto::hash_password(password);
    
    // Hash should not be empty
    EXPECT_FALSE(hash.empty());
    
    // Verify password against the hash
    bool valid = qb::crypto::verify_password(password, hash);
    EXPECT_TRUE(valid);
    
    // Verify with wrong password
    valid = qb::crypto::verify_password("wrong_password", hash);
    EXPECT_FALSE(valid);
    
    // Verify that two hashes of the same password are different
    std::string hash2 = qb::crypto::hash_password(password);
    EXPECT_NE(hash, hash2);
}

// Tests for unique IV generation
TEST_F(CryptoAdvancedTest, UniqueIV) {
    // Generate multiple IVs
    std::vector<std::vector<unsigned char>> ivs;
    
    for (int i = 0; i < 100; i++) {
        ivs.push_back(qb::crypto::generate_unique_iv(12));
    }
    
    // Check that all IVs have the right size
    for (const auto& iv : ivs) {
        EXPECT_EQ(iv.size(), 12);
    }
    
    // Check that all IVs are unique
    for (size_t i = 0; i < ivs.size(); i++) {
        for (size_t j = i + 1; j < ivs.size(); j++) {
            EXPECT_NE(ivs[i], ivs[j]);
        }
    }
}

// Tests for encryption with metadata
TEST_F(CryptoAdvancedTest, EncryptWithMetadata) {
    // Test data
    std::vector<unsigned char> plaintext = {'s', 'e', 'c', 'r', 'e', 't'};
    std::string metadata = "{\"user\":\"alice\",\"timestamp\":123456789}";
    
    // Encrypt
    std::string encrypted = qb::crypto::encrypt_with_metadata(
        plaintext, test_key, metadata);
    
    // Encrypted data should not be empty
    EXPECT_FALSE(encrypted.empty());
    
    // Decrypt
    auto result = qb::crypto::decrypt_with_metadata(
        encrypted, test_key);
    
    // Should have a result
    ASSERT_TRUE(result.has_value());
    
    // Check decrypted data
    EXPECT_EQ(result->first, plaintext);
    EXPECT_EQ(result->second, metadata);
    
    // Test with wrong key
    std::vector<unsigned char> wrong_key = qb::crypto::generate_key(
        qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    auto wrong_result = qb::crypto::decrypt_with_metadata(
        encrypted, wrong_key);
    
    // Should not have a result
    EXPECT_FALSE(wrong_result.has_value());
    
    // Test with tampered metadata
    std::string tampered = encrypted;
    // Replace "alice" with "bob"
    size_t pos = tampered.find("alice");
    if (pos != std::string::npos) {
        tampered.replace(pos, 5, "bob");
    }
    
    auto tampered_result = qb::crypto::decrypt_with_metadata(
        tampered, test_key);
    
    // Authentication should fail
    EXPECT_FALSE(tampered_result.has_value());
}

}  // namespace

// Run all the tests that were declared with TEST()
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 