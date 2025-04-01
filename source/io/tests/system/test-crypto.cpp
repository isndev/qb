/**
 * @file qb/source/io/tests/system/test-crypto.cpp
 * @brief Basic tests for cryptographic functions in the qb IO library
 * 
 * This file contains unit tests for basic cryptographic operations.
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

namespace {

// Test fixture for basic cryptographic functions
class CryptoBasicTest : public ::testing::Test {
protected:
    // Test data
    std::string test_string;
    std::vector<unsigned char> test_data;
    
    void SetUp() override {
        // Initialize test data
        test_string = "Hello, Crypto World!";
        test_data.assign(test_string.begin(), test_string.end());
    }
};

// Tests for base64 encoding/decoding
TEST_F(CryptoBasicTest, Base64) {
    // Encode the test data
    std::string encoded = qb::crypto::base64_encode(test_data.data(), test_data.size());
    
    // Expected result
    std::string expected = "SGVsbG8sIENyeXB0byBXb3JsZCE=";
    
    // Check encoding
    EXPECT_EQ(encoded, expected);
    
    // Decode the encoded string
    std::vector<unsigned char> decoded = qb::crypto::base64_decode(encoded);
    
    // Check decoding
    EXPECT_EQ(decoded, test_data);
}

// Tests for MD5 hashing
TEST_F(CryptoBasicTest, MD5Hash) {
    // Hash the test string
    std::string hash = qb::crypto::to_hex_string(qb::crypto::md5(test_string), qb::crypto::range_hex_lower);
    
    // Expected result (correspondant à la sortie actuelle)
    std::string expected = "39076285a6c5ba8ecb12881f3263987f";
    
    // Check hash
    EXPECT_EQ(hash, expected);
}

// Tests for SHA1 hashing
TEST_F(CryptoBasicTest, SHA1Hash) {
    // Hash the test string
    std::string hash = qb::crypto::to_hex_string(qb::crypto::sha1(test_string), qb::crypto::range_hex_lower);
    
    // Expected result (correspondant à la sortie actuelle)
    std::string expected = "93fcd83c3e94fd6b028c811033333c42e9c5cc6b";
    
    // Check hash
    EXPECT_EQ(hash, expected);
}

// Tests for SHA256 hashing
TEST_F(CryptoBasicTest, SHA256Hash) {
    // Hash the test string
    std::string hash = qb::crypto::to_hex_string(qb::crypto::sha256(test_string), qb::crypto::range_hex_lower);
    
    // Expected result (correspondant à la sortie actuelle)
    std::string expected = "9a15e201db8dbc4fe4ad851cc66e28c650400393ee05932d22132cfae71c803b";
    
    // Check hash
    EXPECT_EQ(hash, expected);
}

// Tests for symmetric encryption/decryption
TEST_F(CryptoBasicTest, SymmetricEncryption) {
    // Generate a key and IV for AES-256-GCM
    std::vector<unsigned char> key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    std::vector<unsigned char> iv = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    // Encrypt the test data
    std::vector<unsigned char> encrypted = qb::crypto::encrypt(
        test_data, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    // Encrypted data should not be empty and should be different from the original
    EXPECT_FALSE(encrypted.empty());
    EXPECT_NE(encrypted, test_data);
    
    // Decrypt the encrypted data
    std::vector<unsigned char> decrypted = qb::crypto::decrypt(
        encrypted, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    // Decrypted data should match the original
    EXPECT_EQ(decrypted, test_data);
    
    // Try to decrypt with wrong key (should fail)
    std::vector<unsigned char> wrong_key = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    std::vector<unsigned char> wrong_decrypt = qb::crypto::decrypt(
        encrypted, wrong_key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    
    // Should return empty vector on authentication failure
    EXPECT_TRUE(wrong_decrypt.empty());
}

// Tests for random string generation
TEST_F(CryptoBasicTest, RandomStringGeneration) {
    // Generate random strings of different types
    std::string numeric = qb::crypto::generate_random_string(10, qb::crypto::range_numeric);
    std::string alpha = qb::crypto::generate_random_string(10, qb::crypto::range_alpha);
    std::string hex = qb::crypto::generate_random_string(10, qb::crypto::range_hex_upper);
    
    // Check lengths
    EXPECT_EQ(numeric.length(), 10);
    EXPECT_EQ(alpha.length(), 10);
    EXPECT_EQ(hex.length(), 10);
    
    // Check character sets
    for (char c : numeric) {
        EXPECT_TRUE(c >= '0' && c <= '9');
    }
    
    for (char c : alpha) {
        EXPECT_TRUE((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    }
    
    for (char c : hex) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    }
    
    // Generate two random strings of the same type and length
    std::string rand1 = qb::crypto::generate_random_string(20, qb::crypto::range_alpha_numeric);
    std::string rand2 = qb::crypto::generate_random_string(20, qb::crypto::range_alpha_numeric);
    
    // They should be different (with very high probability)
    EXPECT_NE(rand1, rand2);
}

// Tests for random bytes generation
TEST_F(CryptoBasicTest, RandomBytesGeneration) {
    // Generate random bytes
    std::vector<unsigned char> random_bytes = qb::crypto::generate_random_bytes(32);
    
    // Check length
    EXPECT_EQ(random_bytes.size(), 32);
    
    // Generate another set of random bytes
    std::vector<unsigned char> random_bytes2 = qb::crypto::generate_random_bytes(32);
    
    // They should be different (with very high probability)
    EXPECT_NE(random_bytes, random_bytes2);
}

// Tests for HMAC-SHA256
TEST_F(CryptoBasicTest, HMACSHA256) {
    // Create a key
    std::vector<unsigned char> key = {'k', 'e', 'y'};
    
    // Compute HMAC
    std::vector<unsigned char> hmac = qb::crypto::hmac_sha256(key, test_string);
    
    // HMAC should not be empty
    EXPECT_FALSE(hmac.empty());
    
    // HMAC with different key should be different
    std::vector<unsigned char> different_key = {'d', 'i', 'f', 'f', 'e', 'r', 'e', 'n', 't'};
    std::vector<unsigned char> different_hmac = qb::crypto::hmac_sha256(different_key, test_string);
    
    EXPECT_NE(hmac, different_hmac);
}

}  // namespace

// Run all the tests that were declared with TEST()
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 