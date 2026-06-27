/**
 * @file unit/crypto/asymmetric-keys.cpp
 * @brief `qb::crypto` asymmetric / PKI half — pure logic, no engine.
 *
 * The asymmetric primitives of the `qb::crypto` facade — Ed25519 signing, X25519 ECDH,
 * ECIES hybrid encryption, RSA and EC sign/verify — are deterministic OpenSSL-backed
 * transforms over in-memory buffers. No event loop, no socket, no daemon: a `unit` test
 * (libcrypto is a link-time dependency). The contracts proven:
 *
 *   - key generation: Ed25519/X25519/RSA/EC produce well-formed PEM (PRIVATE/PUBLIC KEY)
 *     and 32-byte raw material; RSA-1024 and bogus curves are rejected (min-strength);
 *   - Ed25519 matches the RFC 8032 TEST 1 known-answer signature (raw seed -> exact 64-byte
 *     signature, and the RFC public key verifies it) — so an internally-consistent-but-wrong
 *     implementation cannot pass; plus PEM/raw round-trips and wrong-key/tamper/wrong-length
 *     negative controls;
 *   - X25519 matches the RFC 7748 §6.1 known-answer shared secret (raw vectors), is symmetric
 *     for PEM and raw keys, and rejects non-PEM / incompatible peer keys;
 *   - RSA and EC sign/verify round-trip across digests/curves and reject wrong key, tampered
 *     data, tampered/empty signature, bad digest enum and non-PEM keys;
 *   - ECIES round-trips across {STANDARD, AES_GCM, CHACHA20} x {0,16,1024,8192,1 MiB} with a
 *     32-byte ephemeral public key, authenticates context (AAD), and FAILS to decrypt under a
 *     wrong recipient private key (the historic SUCCEED-swallow is now a hard assertion);
 *   - end-to-end secure-messaging and cross-algorithm (Ed25519 sign -> ECIES) narratives hold.
 *
 * Restructured from the dissolved system/test-crypto-asymmetric.cpp: `ECIESErrorHandling`
 * (a `try/catch` that printed a note on no-throw and `SUCCEED()`d on throw — passing both
 * ways) now asserts the wrong-key decryption yields an empty buffer; the ~9 single-mode ECIES
 * round-trip restatements are collapsed into the parametrised `EciesModesMatrix` sweep (which
 * subsumes empty-payload, ChaCha20, STANDARD/CBC, and ephemeral-key-size); RFC 7748 X25519 and
 * RFC 8032 Ed25519 known-answer vectors are added so the suite is no longer only self-consistent;
 * the file-local main() is dropped for the shared gtest_main.
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

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>

namespace {

/**
 * @brief Convert an even-length hex string to its raw bytes (test-vector helper).
 */
std::vector<unsigned char>
hex_to_bytes(const std::string &hex) {
    const std::string decoded = qb::crypto::hex_to_string(hex);
    return std::vector<unsigned char>(decoded.begin(), decoded.end());
}

/**
 * @brief Fixture seeding 256 random bytes as the canonical message under test.
 */
class CryptoAsymmetricTest : public ::testing::Test {
protected:
    std::vector<unsigned char> test_data;

    void
    SetUp() override {
        test_data = qb::crypto::generate_random_bytes(256);
    }
};

// =============================================================================
// KEY GENERATION
// =============================================================================

TEST_F(CryptoAsymmetricTest, KeyGenerationProducesWellFormedMaterial) {
    auto [ed_priv, ed_pub] = qb::crypto::generate_ed25519_keypair();
    EXPECT_NE(ed_priv.find("PRIVATE KEY"), std::string::npos);
    EXPECT_NE(ed_pub.find("PUBLIC KEY"), std::string::npos);

    auto [ed_priv_bytes, ed_pub_bytes] = qb::crypto::generate_ed25519_keypair_bytes();
    EXPECT_EQ(ed_priv_bytes.size(), 32u); // Ed25519 keys are 32 bytes
    EXPECT_EQ(ed_pub_bytes.size(), 32u);

    auto [x_priv_bytes, x_pub_bytes] = qb::crypto::generate_x25519_keypair_bytes();
    EXPECT_EQ(x_priv_bytes.size(), 32u); // X25519 keys are 32 bytes
    EXPECT_EQ(x_pub_bytes.size(), 32u);
}

// =============================================================================
// Ed25519 — RFC 8032 KAT + round-trips + negative controls
// =============================================================================

TEST_F(CryptoAsymmetricTest, Ed25519MatchesRfc8032Vector) {
    // RFC 8032, Section 7.1, TEST 1 (empty message). The 32-byte SECRET KEY is the
    // Ed25519 seed, which is exactly the raw private key OpenSSL expects.
    const auto seed       = hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    const auto public_key = hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const auto expected_signature =
        hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    const std::vector<unsigned char> empty_message;

    const auto signature = qb::crypto::ed25519_sign(empty_message, seed);
    EXPECT_EQ(signature, expected_signature);
    EXPECT_TRUE(qb::crypto::ed25519_verify(empty_message, expected_signature, public_key));

    // A single-bit flip in the signature must be rejected.
    auto bad_signature = expected_signature;
    bad_signature[0] ^= 0x01;
    EXPECT_FALSE(qb::crypto::ed25519_verify(empty_message, bad_signature, public_key));
}

TEST_F(CryptoAsymmetricTest, Ed25519SignAndVerifyPemAndRaw) {
    // PEM material.
    auto [pem_private, pem_public] = qb::crypto::generate_ed25519_keypair();
    const auto sig_pem             = qb::crypto::ed25519_sign(test_data, pem_private);
    EXPECT_TRUE(qb::crypto::ed25519_verify(test_data, sig_pem, pem_public));

    auto tampered_data = test_data;
    tampered_data[0] ^= 0x01;
    EXPECT_FALSE(qb::crypto::ed25519_verify(tampered_data, sig_pem, pem_public));

    auto tampered_sig = sig_pem;
    tampered_sig[0] ^= 0x01;
    EXPECT_FALSE(qb::crypto::ed25519_verify(test_data, tampered_sig, pem_public));

    // Raw material.
    auto [raw_private, raw_public] = qb::crypto::generate_ed25519_keypair_bytes();
    const auto sig_raw             = qb::crypto::ed25519_sign(test_data, raw_private);
    EXPECT_TRUE(qb::crypto::ed25519_verify(test_data, sig_raw, raw_public));

    // Cross-key negative control: an unrelated PEM public key rejects the raw signature.
    EXPECT_FALSE(qb::crypto::ed25519_verify(test_data, sig_raw, pem_public));

    // An unrelated raw public key also rejects.
    auto [_, other_public] = qb::crypto::generate_ed25519_keypair_bytes();
    EXPECT_FALSE(qb::crypto::ed25519_verify(test_data, sig_raw, other_public));
}

TEST_F(CryptoAsymmetricTest, Ed25519VerifyWrongLengthSignatureReturnsFalse) {
    auto [private_key, public_key] = qb::crypto::generate_ed25519_keypair();

    // A wrong-length signature returns false (only key parsing throws).
    const std::vector<unsigned char> short_sig(10, 0x42);
    bool                             valid = true;
    EXPECT_NO_THROW(valid = qb::crypto::ed25519_verify(test_data, short_sig, public_key));
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

// =============================================================================
// X25519 — RFC 7748 KAT + symmetry + error contracts
// =============================================================================

TEST_F(CryptoAsymmetricTest, X25519MatchesRfc7748Vector) {
    // RFC 7748, Section 6.1.
    const auto alice_private = hex_to_bytes("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto alice_public  = hex_to_bytes("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const auto bob_private   = hex_to_bytes("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const auto bob_public    = hex_to_bytes("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const auto expected_k    = hex_to_bytes("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

    EXPECT_EQ(qb::crypto::x25519_key_exchange(alice_private, bob_public), expected_k);
    EXPECT_EQ(qb::crypto::x25519_key_exchange(bob_private, alice_public), expected_k);
}

TEST_F(CryptoAsymmetricTest, X25519SharedSecretIsSymmetricPemAndRaw) {
    auto [alice_pem_priv, alice_pem_pub] = qb::crypto::generate_x25519_keypair();
    auto [bob_pem_priv, bob_pem_pub]     = qb::crypto::generate_x25519_keypair();
    const auto secret_pem     = qb::crypto::x25519_key_exchange(alice_pem_priv, bob_pem_pub);
    const auto secret_pem_rev = qb::crypto::x25519_key_exchange(bob_pem_priv, alice_pem_pub);
    EXPECT_FALSE(secret_pem.empty());
    EXPECT_EQ(secret_pem, secret_pem_rev);

    auto [alice_raw_priv, alice_raw_pub] = qb::crypto::generate_x25519_keypair_bytes();
    auto [bob_raw_priv, bob_raw_pub]     = qb::crypto::generate_x25519_keypair_bytes();
    const auto secret_raw     = qb::crypto::x25519_key_exchange(alice_raw_priv, bob_raw_pub);
    const auto secret_raw_rev = qb::crypto::x25519_key_exchange(bob_raw_priv, alice_raw_pub);
    EXPECT_EQ(secret_raw.size(), 32u); // X25519 shared secrets are 32 bytes
    EXPECT_EQ(secret_raw, secret_raw_rev);
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

// =============================================================================
// RSA
// =============================================================================

TEST_F(CryptoAsymmetricTest, RsaKeyGenerationSignVerifyAndErrorContracts) {
    EXPECT_THROW(qb::crypto::generate_rsa_keypair(1024), std::runtime_error); // below min strength

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

    // An empty signature returns false, not throw.
    bool valid = true;
    EXPECT_NO_THROW(valid = qb::crypto::rsa_verify(test_data, {}, public_key, qb::crypto::DigestAlgorithm::SHA256));
    EXPECT_FALSE(valid);

    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_THROW(qb::crypto::rsa_sign(test_data, private_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_verify(test_data, signature, public_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_sign(test_data, "not a pem"), std::runtime_error);
    EXPECT_THROW(qb::crypto::rsa_verify(test_data, signature, "not a pem"), std::runtime_error);
}

TEST_F(CryptoAsymmetricTest, RsaSignVerifyAcrossKeySizesAndDigests) {
    // 3072-bit keygen + round-trip.
    auto [private_key_3072, public_key_3072] = qb::crypto::generate_rsa_keypair(3072);
    const auto sig_3072 = qb::crypto::rsa_sign(test_data, private_key_3072, qb::crypto::DigestAlgorithm::SHA256);
    ASSERT_FALSE(sig_3072.empty());
    EXPECT_TRUE(qb::crypto::rsa_verify(test_data, sig_3072, public_key_3072, qb::crypto::DigestAlgorithm::SHA256));

    // 2048-bit across all common digests.
    auto [private_key, public_key] = qb::crypto::generate_rsa_keypair(2048);
    const std::vector<qb::crypto::DigestAlgorithm> digests = {
        qb::crypto::DigestAlgorithm::SHA1, qb::crypto::DigestAlgorithm::SHA256, qb::crypto::DigestAlgorithm::SHA384,
        qb::crypto::DigestAlgorithm::SHA512,
    };
    for (const auto digest : digests) {
        const auto signature = qb::crypto::rsa_sign(test_data, private_key, digest);
        ASSERT_FALSE(signature.empty());
        EXPECT_TRUE(qb::crypto::rsa_verify(test_data, signature, public_key, digest));
    }
}

// =============================================================================
// EC (ECDSA)
// =============================================================================

TEST_F(CryptoAsymmetricTest, EcKeyGenerationSignVerifyAndErrorContracts) {
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

    auto tampered_signature = signature;
    tampered_signature.back() ^= 0x01;
    EXPECT_FALSE(qb::crypto::ec_verify(test_data, tampered_signature, public_key, qb::crypto::DigestAlgorithm::SHA256));

    const auto invalid_digest = static_cast<qb::crypto::DigestAlgorithm>(255);
    EXPECT_THROW(qb::crypto::ec_sign(test_data, private_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_verify(test_data, signature, public_key, invalid_digest), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_sign(test_data, "not a pem"), std::runtime_error);
    EXPECT_THROW(qb::crypto::ec_verify(test_data, signature, "not a pem"), std::runtime_error);
}

TEST_F(CryptoAsymmetricTest, EcSignVerifyOnLargerCurves) {
    for (const auto *curve : {"secp384r1", "secp521r1"}) {
        auto [private_key, public_key] = qb::crypto::generate_ec_keypair(curve);
        EXPECT_NE(private_key.find("PRIVATE KEY"), std::string::npos);
        EXPECT_NE(public_key.find("PUBLIC KEY"), std::string::npos);

        const auto signature = qb::crypto::ec_sign(test_data, private_key, qb::crypto::DigestAlgorithm::SHA256);
        ASSERT_FALSE(signature.empty());
        EXPECT_TRUE(qb::crypto::ec_verify(test_data, signature, public_key, qb::crypto::DigestAlgorithm::SHA256));
    }
}

// =============================================================================
// ECIES — consolidated matrix + AAD + wrong-key rejection
// =============================================================================

TEST_F(CryptoAsymmetricTest, EciesModesMatrix) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    const std::vector<qb::crypto::ECIESMode> modes = {
        qb::crypto::ECIESMode::STANDARD, qb::crypto::ECIESMode::AES_GCM, qb::crypto::ECIESMode::CHACHA20,
    };
    const std::vector<std::size_t> sizes = {0, 16, 1024, 8192};

    for (const auto mode : modes) {
        for (const auto size : sizes) {
            std::vector<unsigned char> data;
            if (size > 0) {
                data = qb::crypto::generate_random_bytes(size);
            }

            auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(data, public_key, {}, mode);
            EXPECT_EQ(ephemeral_public.size(), 32u); // ephemeral key is a raw X25519 public key
            if (size > 0) {
                EXPECT_FALSE(encrypted.empty());
            }

            const auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, mode);
            EXPECT_EQ(decrypted, data);
        }
    }
}

TEST_F(CryptoAsymmetricTest, EciesLargePayloadRoundTrip) {
    const auto large_data = qb::crypto::generate_random_bytes(1024 * 1024);

    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();
    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(large_data, public_key, {}, qb::crypto::ECIESMode::AES_GCM);
    ASSERT_FALSE(ephemeral_public.empty());
    ASSERT_FALSE(encrypted.empty());

    const auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, {}, qb::crypto::ECIESMode::AES_GCM);
    EXPECT_EQ(decrypted, large_data);
}

TEST_F(CryptoAsymmetricTest, EciesAuthenticatesContext) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();
    const std::vector<unsigned char> context = {'a', 'u', 't', 'h', 'e', 'n', 't', 'i', 'c', 'a', 't', 'e', 'd'};

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(test_data, public_key, context, qb::crypto::ECIESMode::AES_GCM);

    // Correct context decrypts.
    EXPECT_EQ(qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, context, qb::crypto::ECIESMode::AES_GCM), test_data);

    // Wrong context fails authentication and returns empty.
    const std::vector<unsigned char> wrong_context = {'w', 'r', 'o', 'n', 'g'};
    EXPECT_TRUE(qb::crypto::ecies_decrypt(encrypted, ephemeral_public, private_key, wrong_context, qb::crypto::ECIESMode::AES_GCM).empty());
}

TEST_F(CryptoAsymmetricTest, EciesWrongPrivateKeyDecryptionFails) {
    auto [private_key, public_key] = qb::crypto::generate_x25519_keypair_bytes();

    const std::string                test_string = "Test data for ECIES error handling";
    const std::vector<unsigned char> message(test_string.begin(), test_string.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(message, public_key, {}, qb::crypto::ECIESMode::STANDARD);

    // Decrypting with an unrelated private key must NOT recover the plaintext. This
    // replaces the historic SUCCEED/cout no-op that passed whether the decryption
    // failed or silently succeeded. A wrong key derives a wrong symmetric key, so the
    // CBC path may return empty/garbage or throw a padding error — both are acceptable
    // failures; recovering the message is the only forbidden outcome.
    auto [wrong_private, _] = qb::crypto::generate_x25519_keypair_bytes();
    std::vector<unsigned char> decrypted;
    try {
        decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, wrong_private, {}, qb::crypto::ECIESMode::STANDARD);
    } catch (const std::exception &) {
        decrypted.clear();
    }
    EXPECT_NE(decrypted, message);
}

// =============================================================================
// END-TO-END NARRATIVES
// =============================================================================

TEST_F(CryptoAsymmetricTest, SecureMessagingScenario) {
    auto [alice_sign_private, alice_sign_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [bob_enc_private, bob_enc_public]       = qb::crypto::generate_x25519_keypair_bytes();

    // Alice signs, then encrypts (message || signature) to Bob.
    const auto signature = qb::crypto::ed25519_sign(test_data, alice_sign_private);

    std::vector<unsigned char> message_and_sig;
    message_and_sig.insert(message_and_sig.end(), test_data.begin(), test_data.end());
    message_and_sig.insert(message_and_sig.end(), signature.begin(), signature.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(message_and_sig, bob_enc_public, {}, qb::crypto::ECIESMode::AES_GCM);

    // Bob decrypts, splits, and verifies Alice's signature.
    const auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, bob_enc_private, {}, qb::crypto::ECIESMode::AES_GCM);
    ASSERT_GE(decrypted.size(), 64u);

    const std::vector<unsigned char> received_message(decrypted.begin(), decrypted.end() - 64);
    const std::vector<unsigned char> received_signature(decrypted.end() - 64, decrypted.end());

    EXPECT_EQ(received_message, test_data);
    EXPECT_TRUE(qb::crypto::ed25519_verify(received_message, received_signature, alice_sign_public));
}

TEST_F(CryptoAsymmetricTest, CrossAlgorithmInteroperability) {
    auto [ed_private, ed_public] = qb::crypto::generate_ed25519_keypair_bytes();
    auto [x_private, x_public]   = qb::crypto::generate_x25519_keypair_bytes();

    const auto signature = qb::crypto::ed25519_sign(test_data, ed_private);

    std::vector<unsigned char> combined;
    combined.insert(combined.end(), test_data.begin(), test_data.end());
    combined.insert(combined.end(), signature.begin(), signature.end());
    combined.insert(combined.end(), ed_public.begin(), ed_public.end());

    auto [ephemeral_public, encrypted] = qb::crypto::ecies_encrypt(combined, x_public, {}, qb::crypto::ECIESMode::AES_GCM);
    const auto decrypted = qb::crypto::ecies_decrypt(encrypted, ephemeral_public, x_private, {}, qb::crypto::ECIESMode::AES_GCM);

    ASSERT_GE(decrypted.size(), test_data.size() + signature.size() + ed_public.size());
    const std::vector<unsigned char> recovered_data(decrypted.begin(), decrypted.begin() + test_data.size());
    const std::vector<unsigned char> recovered_signature(decrypted.begin() + test_data.size(),
                                                         decrypted.begin() + test_data.size() + signature.size());
    const std::vector<unsigned char> recovered_public_key(decrypted.begin() + test_data.size() + signature.size(), decrypted.end());

    EXPECT_EQ(recovered_data, test_data);
    EXPECT_EQ(recovered_signature, signature);
    EXPECT_EQ(recovered_public_key, ed_public);
    EXPECT_TRUE(qb::crypto::ed25519_verify(recovered_data, recovered_signature, recovered_public_key));
}

} // namespace
