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

#include <gtest/gtest.h>
#include <qb/io/crypto.h>
#include <iostream>
#include <vector>
#include <string>

namespace {

// Test fixture for asymmetric cryptographic functions
class CryptoAsymmetricTest : public ::testing::Test {
protected:
    // Test data
    std::vector<unsigned char> test_data;
    
    void SetUp() override {
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
    std::vector<unsigned char> signature = qb::crypto::ed25519_sign(test_data, private_key);
    
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
    std::vector<unsigned char> signature = qb::crypto::ed25519_sign(test_data, private_key);
    
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
    auto [bob_private, bob_public] = qb::crypto::generate_x25519_keypair();
    
    // Derive shared secrets
    std::vector<unsigned char> alice_shared = qb::crypto::x25519_key_exchange(
        alice_private, bob_public);
    
    std::vector<unsigned char> bob_shared = qb::crypto::x25519_key_exchange(
        bob_private, alice_public);
    
    // Check that both shared secrets are identical
    EXPECT_EQ(alice_shared, bob_shared);
}

// Tests for X25519 key exchange with raw key bytes
TEST_F(CryptoAsymmetricTest, X25519RawKeyExchange) {
    // Generate two key pairs
    auto [alice_private, alice_public] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_private, bob_public] = qb::crypto::generate_x25519_keypair_bytes();
    
    // Derive shared secrets
    std::vector<unsigned char> alice_shared = qb::crypto::x25519_key_exchange(
        alice_private, bob_public);
    
    std::vector<unsigned char> bob_shared = qb::crypto::x25519_key_exchange(
        bob_private, alice_public);
    
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
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        test_data, public_key, context, mode);
    
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
    auto [alice_sign_private, alice_sign_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [bob_sign_private, bob_sign_public] = qb::crypto::generate_ed25519_keypair_bytes();
    
    // Generate encryption keys for Alice and Bob
    auto [alice_enc_private, alice_enc_public] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_enc_private, bob_enc_public] = qb::crypto::generate_x25519_keypair_bytes();
    
    // Alice wants to send a message to Bob:
    // 1. Sign the message with her private signing key
    std::vector<unsigned char> signature = qb::crypto::ed25519_sign(test_data, alice_sign_private);
    
    // 2. Encrypt the message and signature using Bob's public encryption key
    std::vector<unsigned char> message_and_sig;
    message_and_sig.insert(message_and_sig.end(), test_data.begin(), test_data.end());
    message_and_sig.insert(message_and_sig.end(), signature.begin(), signature.end());
    
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(
        message_and_sig, bob_enc_public, {}, qb::crypto::ECIESMode::AES_GCM);
    
    // Bob receives the message:
    // 1. Decrypt the message using his private encryption key
    std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(
        encrypted, ephemeral_public, bob_enc_private, {}, qb::crypto::ECIESMode::AES_GCM);
    
    // 2. Extract the message and signature
    std::vector<unsigned char> received_message(decrypted.begin(), decrypted.end() - 64);
    std::vector<unsigned char> received_signature(decrypted.end() - 64, decrypted.end());
    
    // 3. Verify the signature using Alice's public signing key
    bool valid = qb::crypto::ed25519_verify(received_message, received_signature, alice_sign_public);
    
    // Check that everything worked correctly
    EXPECT_TRUE(valid);
    EXPECT_EQ(received_message, test_data);
}

}  // namespace

// Run all the tests that were declared with TEST()
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 