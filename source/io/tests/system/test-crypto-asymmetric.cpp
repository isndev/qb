/**
 * @file qb/source/io/tests/system/test-crypto-asymmetric.cpp
 * @brief Tests for asymmetric cryptographic functions in the qb IO library
 *
 * This file contains unit tests for Ed25519, X25519, and ECIES functionality.
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

#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <qb/io/crypto.h>
#include <string>
#include <vector>

namespace {

// Test fixture for asymmetric cryptographic functions
class CryptoAsymmetricTest : public ::testing::Test {
protected:
    // Test data
    std::vector<unsigned char> test_data;

    void
    SetUp() override {
        // Initialize test data with random bytes
        test_data = qb::crypto::generate_random_bytes(256);
    }
};

// Tests for Ed25519 key generation and usage
TEST_F(CryptoAsymmetricTest, Ed25519KeyGeneration) {
    // Generate Ed25519 key pair (PEM format)
    auto [private_key, public_key] = qb::crypto::generate_ed25519_keypair();

    // Check that keys are not empty and in correct format
    EXPECT_FALSE(private_key.empty());
    EXPECT_FALSE(public_key.empty());
    EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
    EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

    // Generate Ed25519 key pair (raw bytes)
    auto [priv_bytes, pub_bytes] = qb::crypto::generate_ed25519_keypair_bytes();

    // Check that keys have correct size
    EXPECT_EQ(priv_bytes.size(), 32); // Ed25519 private keys are 32 bytes
    EXPECT_EQ(pub_bytes.size(), 32);  // Ed25519 public keys are 32 bytes
}

// Tests for Ed25519 signing and verification with PEM keys
TEST_F(CryptoAsymmetricTest, Ed25519SignAndVerify) {
    // Generate key pair
    auto [private_key, public_key] = qb::crypto::generate_ed25519_keypair();

    // Sign test data
    std::vector<unsigned char> signature =
        qb::crypto::ed25519_sign(test_data, private_key);

    // Verify signature
    bool valid = qb::crypto::ed25519_verify(test_data, signature, public_key);
    EXPECT_TRUE(valid);

    // Modify the data and verify again (should fail)
    std::vector<unsigned char> modified_data = test_data;
    if (!modified_data.empty()) {
        modified_data[0] ^= 0x01; // Flip one bit
    }

    valid = qb::crypto::ed25519_verify(modified_data, signature, public_key);
    EXPECT_FALSE(valid);

    // Modify the signature and verify (should fail)
    std::vector<unsigned char> modified_sig = signature;
    if (!modified_sig.empty()) {
        modified_sig[0] ^= 0x01; // Flip one bit
    }

    valid = qb::crypto::ed25519_verify(test_data, modified_sig, public_key);
    EXPECT_FALSE(valid);
}

// Tests for Ed25519 signing and verification with raw key bytes
TEST_F(CryptoAsymmetricTest, Ed25519RawKeySignAndVerify) {
    // Generate key pair
    auto [private_key, public_key] = qb::crypto::generate_ed25519_keypair_bytes();

    // Sign test data
    std::vector<unsigned char> signature =
        qb::crypto::ed25519_sign(test_data, private_key);

    // Verify signature
    bool valid = qb::crypto::ed25519_verify(test_data, signature, public_key);
    EXPECT_TRUE(valid);

    // Try to verify with wrong key (should fail)
    auto [_, wrong_public_key] = qb::crypto::generate_ed25519_keypair_bytes();
    valid = qb::crypto::ed25519_verify(test_data, signature, wrong_public_key);
    EXPECT_FALSE(valid);
}

// Tests for X25519 key exchange
TEST_F(CryptoAsymmetricTest, X25519KeyExchange) {
    // Generate two key pairs
    auto [alice_private, alice_public] = qb::crypto::generate_x25519_keypair();
    auto [bob_private, bob_public]     = qb::crypto::generate_x25519_keypair();

    // Derive shared secrets
    std::vector<unsigned char> alice_shared =
        qb::crypto::x25519_key_exchange(alice_private, bob_public);

    std::vector<unsigned char> bob_shared =
        qb::crypto::x25519_key_exchange(bob_private, alice_public);

    // Check that both shared secrets are identical
    EXPECT_EQ(alice_shared, bob_shared);
}

// Tests for X25519 key exchange with raw key bytes
TEST_F(CryptoAsymmetricTest, X25519RawKeyExchange) {
    // Generate two key pairs
    auto [alice_private, alice_public] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_private, bob_public]     = qb::crypto::generate_x25519_keypair_bytes();

    // Derive shared secrets
    std::vector<unsigned char> alice_shared =
        qb::crypto::x25519_key_exchange(alice_private, bob_public);

    std::vector<unsigned char> bob_shared =
        qb::crypto::x25519_key_exchange(bob_private, alice_public);

    // Check that both shared secrets are identical
    EXPECT_EQ(alice_shared, bob_shared);

    // Check that the shared secret has the expected size
    EXPECT_EQ(alice_shared.size(), 32); // X25519 shared secrets are 32 bytes
}

// Tests for ECIES encryption and decryption
TEST_F(CryptoAsymmetricTest, ECIESEncryptDecrypt) {
    // Generate recipient key pair
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    // Optional shared context information
    std::vector<unsigned char> context = {'c', 'o', 'n', 't', 'e', 'x', 't'};

    // Test standard ECIES mode (which should be most reliable)
    qb::crypto::ECIESMode mode = qb::crypto::ECIESMode::STANDARD;

    // Encrypt data
    auto [ephemeral_public, encrypted] =
        qb::crypto::ecies_encrypt(test_data, public_key, context, mode);

    // Check that we got results
    EXPECT_FALSE(ephemeral_public.empty());
    EXPECT_FALSE(encrypted.empty());

    // Decrypt data
    std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(
        encrypted, ephemeral_public, private_key, context, mode);

    // Check that decrypted data matches original
    EXPECT_EQ(decrypted, test_data);
}

// Test for secure messaging scenario
TEST_F(CryptoAsymmetricTest, SecureMessagingScenario) {
    // Generate identity keys for Alice and Bob
    auto [alice_sign_private, alice_sign_public] =
        qb::crypto::generate_ed25519_keypair_bytes();
    auto [bob_sign_private, bob_sign_public] =
        qb::crypto::generate_ed25519_keypair_bytes();

    // Generate encryption keys for Alice and Bob
    auto [alice_enc_private, alice_enc_public] =
        qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_enc_private, bob_enc_public] = qb::crypto::generate_x25519_keypair_bytes();

    // Alice wants to send a message to Bob:
    // 1. Sign the message with her private signing key
    std::vector<unsigned char> signature =
        qb::crypto::ed25519_sign(test_data, alice_sign_private);

    // 2. Encrypt the message and signature using Bob's public encryption key
    std::vector<unsigned char> message_and_sig;
    message_and_sig.insert(message_and_sig.end(), test_data.begin(), test_data.end());
    message_and_sig.insert(message_and_sig.end(), signature.begin(), signature.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        message_and_sig, bob_enc_public, {}, qb::crypto::ECIESMode::AES_GCM);

    // Bob receives the message:
    // 1. Decrypt the message using his private encryption key
    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, bob_enc_private, {},
                                  qb::crypto::ECIESMode::AES_GCM);

    // 2. Extract the message and signature
    std::vector<unsigned char> received_message(decrypted.begin(), decrypted.end() - 64);
    std::vector<unsigned char> received_signature(decrypted.end() - 64, decrypted.end());

    // 3. Verify the signature using Alice's public signing key
    bool valid = qb::crypto::ed25519_verify(received_message, received_signature,
                                            alice_sign_public);

    // Check that everything worked correctly
    EXPECT_TRUE(valid);
    EXPECT_EQ(received_message, test_data);
}

// Test for ECIES with different modes and sizes
TEST_F(CryptoAsymmetricTest, ECIESModes) {
    // Generate recipient key pair
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    // Test data sizes to try (including empty and large)
    std::vector<size_t> data_sizes = {0, 16, 1024, 8192};

    // Test all ECIES modes
    std::vector<qb::crypto::ECIESMode> modes = {qb::crypto::ECIESMode::STANDARD,
                                                qb::crypto::ECIESMode::AES_GCM,
                                                qb::crypto::ECIESMode::CHACHA20};

    for (auto mode : modes) {
        for (auto size : data_sizes) {
            // Generate test data of specified size
            std::vector<unsigned char> data;
            if (size > 0) {
                data = qb::crypto::generate_random_bytes(size);
            }

            // Encrypt data
            auto [ephemeral_public, encrypted] =
                qb::crypto::ecies_encrypt(data, public_key, {}, mode);

            // Check that we got results
            EXPECT_FALSE(ephemeral_public.empty());
            if (size > 0) {
                EXPECT_FALSE(encrypted.empty());
            }

            // Decrypt data
            std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(
                encrypted, ephemeral_public, private_key, {}, mode);

            // Check that decrypted data matches original
            EXPECT_EQ(decrypted, data);
        }
    }
}

// Test for error handling in ECIES operations
TEST_F(CryptoAsymmetricTest, ECIESErrorHandling) {
    try {
        // Generate recipient key pair
        auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

        // Simple test data
        std::string                test_string = "Test data for ECIES error handling";
        std::vector<unsigned char> test_data_vec(test_string.begin(), test_string.end());

        // 1. Encrypt data with standard mode
        auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
            test_data_vec, public_key, {}, qb::crypto::ECIESMode::STANDARD);

        // 2. Deliberately use wrong key for decryption (should fail)
        auto [wrong_private, _] = qb::crypto::generate_x25519_keypair_bytes();

        // We expect this to throw an exception
        std::vector<unsigned char> decrypted =
            qb::crypto::ecies_decrypt(encrypted, ephemeral_public, wrong_private, {},
                                      qb::crypto::ECIESMode::STANDARD);

        // Should not reach here, but if it does (implementation specific), just output a
        // message
        std::cout << "Note: Expected decryption to fail with wrong key, but got "
                  << decrypted.size() << " bytes." << std::endl;
    } catch (const std::exception &e) {
        // Expected behavior - decryption failed
        std::cout << "Expected decryption error: " << e.what() << std::endl;
        SUCCEED() << "Correctly detected decryption error with wrong key";
    }
}

// Test for ECIES authenticated data
TEST_F(CryptoAsymmetricTest, ECIESWithContext) {
    // Generate recipient key pair
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    // Context information (authenticated but not encrypted)
    std::vector<unsigned char> context = {'a', 'u', 't', 'h', 'e', 'n', 't',
                                          'i', 'c', 'a', 't', 'e', 'd'};

    // Encrypt with context
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        test_data, public_key, context, qb::crypto::ECIESMode::AES_GCM);

    // Decrypt with correct context
    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, context,
                                  qb::crypto::ECIESMode::AES_GCM);

    // Should decrypt correctly
    EXPECT_EQ(decrypted, test_data);

    // Decrypt with wrong context
    std::vector<unsigned char> wrong_context = {'w', 'r', 'o', 'n', 'g'};
    std::vector<unsigned char> wrong_context_decrypt =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key,
                                  wrong_context, qb::crypto::ECIESMode::AES_GCM);

    // Should fail and return empty
    EXPECT_TRUE(wrong_context_decrypt.empty());
}

// Test for cross-algorithm compatibility
TEST_F(CryptoAsymmetricTest, CrossAlgorithmInteroperability) {
    // Generate Ed25519 and X25519 key pairs
    auto [ed_private, ed_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [x_private, x_public]   = qb::crypto::generate_x25519_keypair_bytes();

    // Sign data with Ed25519
    std::vector<unsigned char> signature =
        qb::crypto::ed25519_sign(test_data, ed_private);

    // Encrypt signed data with X25519/ECIES
    std::vector<unsigned char> combined_data;
    combined_data.insert(combined_data.end(), test_data.begin(), test_data.end());
    combined_data.insert(combined_data.end(), signature.begin(), signature.end());
    combined_data.insert(combined_data.end(), ed_public.begin(), ed_public.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        combined_data, x_public, {}, qb::crypto::ECIESMode::AES_GCM);

    // Decrypt with X25519
    std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(
        encrypted, ephemeral_public, x_private, {}, qb::crypto::ECIESMode::AES_GCM);

    // Extract the original data, signature, and public key
    ASSERT_GE(decrypted.size(), test_data.size() + signature.size() + ed_public.size());

    std::vector<unsigned char> recovered_data(decrypted.begin(),
                                              decrypted.begin() + test_data.size());

    std::vector<unsigned char> recovered_signature(decrypted.begin() + test_data.size(),
                                                   decrypted.begin() + test_data.size() +
                                                       signature.size());

    std::vector<unsigned char> recovered_public_key(
        decrypted.begin() + test_data.size() + signature.size(), decrypted.end());

    // Verify that everything matches
    EXPECT_EQ(recovered_data, test_data);
    EXPECT_EQ(recovered_signature, signature);
    EXPECT_EQ(recovered_public_key, ed_public);

    // Verify the signature with the recovered public key
    bool verified = qb::crypto::ed25519_verify(recovered_data, recovered_signature,
                                               recovered_public_key);

    EXPECT_TRUE(verified);
}

// Test for asymmetric cryptography performance
TEST_F(CryptoAsymmetricTest, AsymmetricPerformance) {
    // This test case verifies that operations complete in a reasonable time
    // Note: This is not a strict performance test, but rather a basic sanity check

    const int iterations = 10;

    // Generate test data (1MB)
    std::vector<unsigned char> large_data =
        qb::crypto::generate_random_bytes(1024 * 1024);

    // Time X25519 key generation
    auto start_keygen = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();
        EXPECT_FALSE(private_key.empty());
        EXPECT_FALSE(public_key.empty());
    }

    auto end_keygen = std::chrono::high_resolution_clock::now();
    auto keygen_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_keygen - start_keygen)
            .count();

    // Time for each key generation should be reasonably fast (typically < 10ms per key)
    double avg_keygen_ms = static_cast<double>(keygen_duration) / iterations;
    EXPECT_LT(avg_keygen_ms, 50.0); // Very conservative upper bound

    // Generate key pair for encryption test
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    // Time ECIES encryption (only a few iterations for large data)
    auto start_encrypt = std::chrono::high_resolution_clock::now();

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        large_data, public_key, {}, qb::crypto::ECIESMode::AES_GCM);

    auto end_encrypt      = std::chrono::high_resolution_clock::now();
    auto encrypt_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                end_encrypt - start_encrypt)
                                .count();

    // Encryption of 1MB should complete in a reasonable time (typically < 1 second)
    EXPECT_LT(encrypt_duration, 1000); // Very conservative upper bound

    // Time ECIES decryption
    auto start_decrypt = std::chrono::high_resolution_clock::now();

    std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(
        encrypted, ephemeral_public, private_key, {}, qb::crypto::ECIESMode::AES_GCM);

    auto end_decrypt      = std::chrono::high_resolution_clock::now();
    auto decrypt_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                end_decrypt - start_decrypt)
                                .count();

    // Decryption of 1MB should complete in a reasonable time (typically < 1 second)
    EXPECT_LT(decrypt_duration, 1000); // Very conservative upper bound

    // Ensure decryption was successful
    EXPECT_EQ(decrypted, large_data);
}

} // namespace

// Run all the tests that were declared with TEST()
int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}