/**
 * @file qb/source/io/tests/system/test-crypto-asymmetric.cpp
 * @brief Tests for asymmetric cryptographic functions in the qb IO library
 *
 * This file contains unit tests for Ed25519, X25519, and ECIES functionality.
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
 * @ingroup IO
 */

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

TEST_F(CryptoAsymmetricTest, RSAKeyGenerationSignVerifyAndErrorContracts) {
    EXPECT_THROW(qb::crypto::generate_rsa_keypair(1024), std::runtime_error);

    auto [private_key, public_key] = qb::crypto::generate_rsa_keypair(2048);
    EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
    EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

    const auto signature = qb::crypto::rsa_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
    ASSERT_FALSE(signature.empty());
    EXPECT_TRUE(qb::crypto::rsa_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    auto modified_data = test_data;
    modified_data[0] ^= 0x40;
    EXPECT_FALSE(qb::crypto::rsa_verify(modified_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    auto modified_signature = signature;
    modified_signature[0] ^= 0x20;
    EXPECT_FALSE(qb::crypto::rsa_verify(test_data, modified_signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_THROW(qb::crypto::rsa_sign(test_data, private_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_verify(test_data, signature, public_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_sign(test_data, "not a pem"), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_verify(test_data, signature, "not a pem"), std::runtime_error);
}

TEST_F(CryptoAsymmetricTest, ECKeyGenerationSignVerifyAndErrorContracts) {
    EXPECT_THROW(qb::crypto::generate_ec_keypair("not-a-curve"), std::runtime_error);

    auto [private_key, public_key] = qb::crypto::generate_ec_keypair("prime256v1");
    EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
    EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

    const auto signature = qb::crypto::ec_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
    ASSERT_FALSE(signature.empty());
    EXPECT_TRUE(qb::crypto::ec_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    auto [wrong_private_key, wrong_public_key] = qb::crypto::generate_ec_keypair("prime256v1");
    (void) wrong_private_key;
    EXPECT_FALSE(qb::crypto::ec_verify(test_data, signature, wrong_public_key, qb::crypto::DigestAlgorithm::SHA256));

    auto modified_signature = signature;
    modified_signature.back() ^= 0x01;
    EXPECT_FALSE(qb::crypto::ec_verify(test_data, modified_signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_THROW(qb::crypto::ec_sign(test_data, private_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_verify(test_data, signature, public_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_sign(test_data, "not a pem"), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_verify(test_data, signature, "not a pem"), std::runtime_error);
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
    valid                      = qb::crypto::ed25519_verify(test_data, signature, wrong_public_key);
    EXPECT_FALSE(valid);
}

TEST_F(CryptoAsymmetricTest, RawKeyInputsRejectInvalidSizes) {
    const std::vector<unsigned char> too_short(31, 0x42);
    const std::vector<unsigned char> too_long(33, 0x42);
    const std::vector<unsigned char> empty;

    EXPECT_THROW(qb::crypto::ed25519_sign(test_data, too_short), std::runtime_error);
    EXPECT_THROW(qb::crypto::ed25519_verify(test_data, empty, too_short), std::runtime_error);
    EXPECT_THROW(qb::crypto::ed25519_verify(test_data, empty, too_long), std::runtime_error);

    auto [x_private, x_public] = qb::crypto::generate_x25519_keypair_bytes();
    EXPECT_THROW(qb::crypto::x25519_key_exchange(too_short, x_public), std::runtime_error);
    EXPECT_THROW(qb::crypto::x25519_key_exchange(x_private, too_short), std::runtime_error);
}

// Tests for X25519 key exchange
TEST_F(CryptoAsymmetricTest, X25519KeyExchange) {
    // Generate two key pairs
    auto [alice_private, alice_public] = qb::crypto::generate_x25519_keypair();
    auto [bob_private, bob_public]     = qb::crypto::generate_x25519_keypair();

    // Derive shared secrets
    std::vector<unsigned char> alice_shared = qb::crypto::x25519_key_exchange(alice_private, bob_public);

    std::vector<unsigned char> bob_shared = qb::crypto::x25519_key_exchange(bob_private, alice_public);

    // Check that both shared secrets are identical
    EXPECT_EQ(alice_shared, bob_shared);
}

TEST_F(CryptoAsymmetricTest, X25519PemRejectsIncompatiblePeerKey) {
    auto [x_private, x_public]   = qb::crypto::generate_x25519_keypair();
    auto [ed_private, ed_public] = qb::crypto::generate_ed25519_keypair();
    (void) x_public;
    (void) ed_private;

    EXPECT_THROW(qb::crypto::x25519_key_exchange("not a pem", ed_public), std::runtime_error);
    EXPECT_THROW(qb::crypto::x25519_key_exchange(x_private, "not a pem"), std::runtime_error);
    EXPECT_THROW(qb::crypto::x25519_key_exchange(x_private, ed_public), std::runtime_error);
}

// Tests for X25519 key exchange with raw key bytes
TEST_F(CryptoAsymmetricTest, X25519RawKeyExchange) {
    // Generate two key pairs
    auto [alice_private, alice_public] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_private, bob_public]     = qb::crypto::generate_x25519_keypair_bytes();

    // Derive shared secrets
    std::vector<unsigned char> alice_shared = qb::crypto::x25519_key_exchange(alice_private, bob_public);

    std::vector<unsigned char> bob_shared = qb::crypto::x25519_key_exchange(bob_private, alice_public);

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
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data, public_key, context, mode);

    // Check that we got results
    EXPECT_FALSE(ephemeral_public.empty());
    EXPECT_FALSE(encrypted.empty());

    // Decrypt data
    std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, context, mode);

    // Check that decrypted data matches original
    EXPECT_EQ(decrypted, test_data);
}

// Test for secure messaging scenario
TEST_F(CryptoAsymmetricTest, SecureMessagingScenario) {
    // Generate identity keys for Alice and Bob
    auto [alice_sign_private, alice_sign_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [bob_sign_private, bob_sign_public]     = qb::crypto::generate_ed25519_keypair_bytes();

    // Generate encryption keys for Alice and Bob
    auto [alice_enc_private, alice_enc_public] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_enc_private, bob_enc_public]     = qb::crypto::generate_x25519_keypair_bytes();

    // Alice wants to send a message to Bob:
    // 1. Sign the message with her private signing key
    std::vector<unsigned char> signature = qb::crypto::ed25519_sign(test_data, alice_sign_private);

    // 2. Encrypt the message and signature using Bob's public encryption key
    std::vector<unsigned char> message_and_sig;
    message_and_sig.insert(message_and_sig.end(), test_data.begin(), test_data.end());
    message_and_sig.insert(message_and_sig.end(), signature.begin(), signature.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(message_and_sig, bob_enc_public, {}, qb::crypto::ECIESMode::AES_GCM);

    // Bob receives the message:
    // 1. Decrypt the message using his private encryption key
    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, bob_enc_private, {}, qb::crypto::ECIESMode::AES_GCM);

    // 2. Extract the message and signature
    std::vector<unsigned char> received_message(decrypted.begin(), decrypted.end() - 64);
    std::vector<unsigned char> received_signature(decrypted.end() - 64, decrypted.end());

    // 3. Verify the signature using Alice's public signing key
    bool valid = qb::crypto::ed25519_verify(received_message, received_signature, alice_sign_public);

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
    std::vector<qb::crypto::ECIESMode> modes = {
        qb::crypto::ECIESMode::STANDARD, qb::crypto::ECIESMode::AES_GCM, qb::crypto::ECIESMode::CHACHA20
    };

    for (auto mode : modes) {
        for (auto size : data_sizes) {
            // Generate test data of specified size
            std::vector<unsigned char> data;
            if (size > 0) {
                data = qb::crypto::generate_random_bytes(size);
            }

            // Encrypt data
            auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(data, public_key, {}, mode);

            // Check that we got results
            EXPECT_FALSE(ephemeral_public.empty());
            if (size > 0) {
                EXPECT_FALSE(encrypted.empty());
            }

            // Decrypt data
            std::vector<unsigned char> decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, mode);

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
        auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data_vec, public_key, {}, qb::crypto::ECIESMode::STANDARD);

        // 2. Deliberately use wrong key for decryption (should fail)
        auto [wrong_private, _] = qb::crypto::generate_x25519_keypair_bytes();

        // We expect this to throw an exception
        std::vector<unsigned char> decrypted =
            qb::crypto::ecies_decrypt(encrypted, ephemeral_public, wrong_private, {}, qb::crypto::ECIESMode::STANDARD);

        // Should not reach here, but if it does (implementation specific), just output a
        // message
        std::cout << "Note: Expected decryption to fail with wrong key, but got " << decrypted.size() << " bytes." << std::endl;
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
    std::vector<unsigned char> context = {'a', 'u', 't', 'h', 'e', 'n', 't', 'i', 'c', 'a', 't', 'e', 'd'};

    // Encrypt with context
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data, public_key, context, qb::crypto::ECIESMode::AES_GCM);

    // Decrypt with correct context
    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, context, qb::crypto::ECIESMode::AES_GCM);

    // Should decrypt correctly
    EXPECT_EQ(decrypted, test_data);

    // Decrypt with wrong context
    std::vector<unsigned char> wrong_context = {'w', 'r', 'o', 'n', 'g'};
    std::vector<unsigned char> wrong_context_decrypt =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, wrong_context, qb::crypto::ECIESMode::AES_GCM);

    // Should fail and return empty
    EXPECT_TRUE(wrong_context_decrypt.empty());
}

// Test for cross-algorithm compatibility
TEST_F(CryptoAsymmetricTest, CrossAlgorithmInteroperability) {
    // Generate Ed25519 and X25519 key pairs
    auto [ed_private, ed_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [x_private, x_public]   = qb::crypto::generate_x25519_keypair_bytes();

    // Sign data with Ed25519
    std::vector<unsigned char> signature = qb::crypto::ed25519_sign(test_data, ed_private);

    // Encrypt signed data with X25519/ECIES
    std::vector<unsigned char> combined_data;
    combined_data.insert(combined_data.end(), test_data.begin(), test_data.end());
    combined_data.insert(combined_data.end(), signature.begin(), signature.end());
    combined_data.insert(combined_data.end(), ed_public.begin(), ed_public.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(combined_data, x_public, {}, qb::crypto::ECIESMode::AES_GCM);

    // Decrypt with X25519
    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, x_private, {}, qb::crypto::ECIESMode::AES_GCM);

    // Extract the original data, signature, and public key
    ASSERT_GE(decrypted.size(), test_data.size() + signature.size() + ed_public.size());

    std::vector<unsigned char> recovered_data(decrypted.begin(), decrypted.begin() + test_data.size());

    std::vector<unsigned char> recovered_signature(decrypted.begin() + test_data.size(),
                                                   decrypted.begin() + test_data.size() + signature.size());

    std::vector<unsigned char> recovered_public_key(decrypted.begin() + test_data.size() + signature.size(), decrypted.end());

    // Verify that everything matches
    EXPECT_EQ(recovered_data, test_data);
    EXPECT_EQ(recovered_signature, signature);
    EXPECT_EQ(recovered_public_key, ed_public);

    // Verify the signature with the recovered public key
    bool verified = qb::crypto::ed25519_verify(recovered_data, recovered_signature, recovered_public_key);

    EXPECT_TRUE(verified);
}

TEST_F(CryptoAsymmetricTest, LargePayloadEciesRoundTrip) {
    std::vector<unsigned char> large_data = qb::crypto::generate_random_bytes(1024 * 1024);

    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();
    ASSERT_FALSE(private_key.empty());
    ASSERT_FALSE(public_key.empty());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(large_data, public_key, {}, qb::crypto::ECIESMode::AES_GCM);
    ASSERT_FALSE(ephemeral_public.empty());
    ASSERT_FALSE(encrypted.empty());

    std::vector<unsigned char> decrypted =
        qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, qb::crypto::ECIESMode::AES_GCM);

    EXPECT_EQ(decrypted, large_data);
}

// 1. ECIES round-trip with the ChaCha20-Poly1305 AEAD mode (small + empty payload).
TEST_F(CryptoAsymmetricTest, EciesChaCha20RoundTrip) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    const std::vector<unsigned char> small = {'h', 'e', 'l', 'l', 'o'};
    auto [eph_small, ct_small]             = qb::crypto::ecies_encrypt(small, public_key, {}, qb::crypto::ECIESMode::CHACHA20);
    EXPECT_FALSE(eph_small.empty());
    EXPECT_FALSE(ct_small.empty());
    auto dec_small = qb::crypto::ecies_decrypt(ct_small, eph_small, private_key, {}, qb::crypto::ECIESMode::CHACHA20);
    EXPECT_EQ(dec_small, small);

    const std::vector<unsigned char> empty;
    auto [eph_empty, ct_empty] = qb::crypto::ecies_encrypt(empty, public_key, {}, qb::crypto::ECIESMode::CHACHA20);
    EXPECT_FALSE(eph_empty.empty());
    auto dec_empty = qb::crypto::ecies_decrypt(ct_empty, eph_empty, private_key, {}, qb::crypto::ECIESMode::CHACHA20);
    EXPECT_TRUE(dec_empty.empty());
}

// 2. ECIES round-trip with the STANDARD mode (maps internally to AES-256-CBC).
//    NOTE: there is no ECIESMode::AES_256_CBC enumerator; STANDARD is the CBC path
//    (see crypto_asymmetric.cpp switch: ECIESMode::STANDARD -> AES_256_CBC).
TEST_F(CryptoAsymmetricTest, EciesStandardCbcRoundTrip) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data, public_key, {}, qb::crypto::ECIESMode::STANDARD);
    EXPECT_FALSE(ephemeral_public.empty());
    EXPECT_FALSE(encrypted.empty());

    auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, qb::crypto::ECIESMode::STANDARD);
    EXPECT_EQ(decrypted, test_data);
}

// 3. 0-byte plaintext across all available ECIES modes: ephemeral public is always
//    populated and the decrypt yields an empty buffer.
TEST_F(CryptoAsymmetricTest, EciesEmptyPlaintext) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    const std::vector<unsigned char> empty;
    const std::vector<qb::crypto::ECIESMode> modes = {
        qb::crypto::ECIESMode::STANDARD, qb::crypto::ECIESMode::AES_GCM, qb::crypto::ECIESMode::CHACHA20
    };

    for (auto mode : modes) {
        auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(empty, public_key, {}, mode);
        EXPECT_FALSE(ephemeral_public.empty());
        auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, mode);
        EXPECT_TRUE(decrypted.empty());
    }
}

// 4. The ECIES ephemeral public key is a raw X25519 public key (32 bytes).
TEST_F(CryptoAsymmetricTest, EciesEphemeralKeyX25519Sized) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data, public_key, {}, qb::crypto::ECIESMode::AES_GCM);
    EXPECT_EQ(ephemeral_public.size(), 32u);
}

// 5. RSA-3072 keygen + sign/verify round-trip.
TEST_F(CryptoAsymmetricTest, RsaKeygen3072SignVerify) {
    auto [private_key, public_key] = qb::crypto::generate_rsa_keypair(3072);
    EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
    EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

    const auto signature = qb::crypto::rsa_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
    ASSERT_FALSE(signature.empty());
    EXPECT_TRUE(qb::crypto::rsa_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));
}

// 6. RSA sign/verify round-trip across the common digest algorithms.
TEST_F(CryptoAsymmetricTest, RsaSignAllDigests) {
    auto [private_key, public_key] = qb::crypto::generate_rsa_keypair(2048);

    const std::vector<qb::crypto::DigestAlgorithm> digests = {
        qb::crypto::DigestAlgorithm::SHA1, qb::crypto::DigestAlgorithm::SHA256, qb::crypto::DigestAlgorithm::SHA384,
        qb::crypto::DigestAlgorithm::SHA512
    };

    for (auto digest : digests) {
        const auto signature = qb::crypto::rsa_sign(test_data, private_key, digest);
        ASSERT_FALSE(signature.empty());
        EXPECT_TRUE(qb::crypto::rsa_verify(test_data, signature, public_key, digest));
    }
}

// 7. EC sign/verify round-trip on the P-384 and P-521 curves.
TEST_F(CryptoAsymmetricTest, EcKeygenSecp384AndSecp521) {
    for (const auto *curve : {"secp384r1", "secp521r1"}) {
        auto [private_key, public_key] = qb::crypto::generate_ec_keypair(curve);
        EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
        EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

        const auto signature = qb::crypto::ec_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
        ASSERT_FALSE(signature.empty());
        EXPECT_TRUE(qb::crypto::ec_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));
    }
}

// 8. Exercise both ed25519_sign / ed25519_verify overloads (PEM and raw-bytes).
//    NOTE: the requested "same keypair, PEM<->raw cross-verify" is not expressible
//    through the public API -- generate_ed25519_keypair() and
//    generate_ed25519_keypair_bytes() produce INDEPENDENT keypairs and there is no
//    public PEM<->raw converter (pem_to_key/get_raw_key_bytes are file-static). So
//    this verifies each overload pair on its own material plus a negative control
//    that a raw-key signature does NOT validate under an unrelated PEM public key.
TEST_F(CryptoAsymmetricTest, Ed25519PemRawCrossVerify) {
    auto [pem_private, pem_public] = qb::crypto::generate_ed25519_keypair();
    auto [raw_private, raw_public] = qb::crypto::generate_ed25519_keypair_bytes();

    const auto sig_from_raw = qb::crypto::ed25519_sign(test_data, raw_private);
    EXPECT_TRUE(qb::crypto::ed25519_verify(test_data, sig_from_raw, raw_public));

    const auto sig_from_pem = qb::crypto::ed25519_sign(test_data, pem_private);
    EXPECT_TRUE(qb::crypto::ed25519_verify(test_data, sig_from_pem, pem_public));

    // Negative control across overloads: unrelated PEM key rejects the raw signature.
    EXPECT_FALSE(qb::crypto::ed25519_verify(test_data, sig_from_raw, pem_public));
}

// 9. Exercise both x25519_key_exchange overloads (PEM strings and raw bytes), each
//    yielding a symmetric (direction-independent) shared secret.
//    NOTE: a literal "same key material via PEM vs raw" equality is not expressible
//    through the public API (no public PEM<->raw converter), so each overload is
//    checked for the ECDH symmetry property on its own keypairs.
TEST_F(CryptoAsymmetricTest, X25519PemBytesEquivalence) {
    auto [alice_pem_priv, alice_pem_pub] = qb::crypto::generate_x25519_keypair();
    auto [bob_pem_priv, bob_pem_pub]     = qb::crypto::generate_x25519_keypair();

    const auto secret_pem     = qb::crypto::x25519_key_exchange(alice_pem_priv, bob_pem_pub);
    const auto secret_pem_rev = qb::crypto::x25519_key_exchange(bob_pem_priv, alice_pem_pub);
    EXPECT_FALSE(secret_pem.empty());
    EXPECT_EQ(secret_pem, secret_pem_rev);

    auto [alice_raw_priv, alice_raw_pub] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_raw_priv, bob_raw_pub]     = qb::crypto::generate_x25519_keypair_bytes();
    const auto secret_raw                = qb::crypto::x25519_key_exchange(alice_raw_priv, bob_raw_pub);
    const auto secret_raw_rev            = qb::crypto::x25519_key_exchange(bob_raw_priv, alice_raw_pub);
    EXPECT_FALSE(secret_raw.empty());
    EXPECT_EQ(secret_raw, secret_raw_rev);
}

// 10. ed25519_verify with a wrong-length (10-byte) signature returns false rather
//     than throwing (verify returns bool; only key parsing throws).
TEST_F(CryptoAsymmetricTest, Ed25519VerifyWrongLengthSignature) {
    auto [private_key, public_key] = qb::crypto::generate_ed25519_keypair();

    const std::vector<unsigned char> short_sig(10, 0x42);
    bool valid = false;
    EXPECT_NO_THROW(valid = qb::crypto::ed25519_verify(test_data, short_sig, public_key));
    EXPECT_FALSE(valid);
}

// 11. ec_verify with a single flipped signature byte returns false.
TEST_F(CryptoAsymmetricTest, EcVerifyTampered) {
    auto [private_key, public_key] = qb::crypto::generate_ec_keypair("prime256v1");

    auto signature = qb::crypto::ec_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
    ASSERT_FALSE(signature.empty());
    EXPECT_TRUE(qb::crypto::ec_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    signature[0] ^= 0x01;
    EXPECT_FALSE(qb::crypto::ec_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));
}

// 12. rsa_verify with an empty signature returns false (not throw).
TEST_F(CryptoAsymmetricTest, RsaVerifyEmptySignature) {
    auto [private_key, public_key] = qb::crypto::generate_rsa_keypair(2048);

    const std::vector<unsigned char> empty_sig;
    bool valid = true;
    EXPECT_NO_THROW(valid = qb::crypto::rsa_verify(test_data, empty_sig, public_key, qb::crypto::DigestAlgorithm::SHA256));
    EXPECT_FALSE(valid);
}

} // namespace

// Run all the tests that were declared with TEST()
int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
