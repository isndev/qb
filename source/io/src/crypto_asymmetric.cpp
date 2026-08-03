/**
 * @file qb/io/crypto_asymmetric.cpp
 * @brief Implementation of asymmetric cryptographic utilities for the QB IO library
 *
 * This file provides implementations of modern asymmetric cryptographic operations such
 * as:
 * - Ed25519 for digital signatures
 * - X25519 for key exchange
 * - ECIES (Elliptic Curve Integrated Encryption Scheme) for hybrid encryption
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

#include <cstring>
#include <fstream>
#include <memory>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <qb/io/crypto.h>
#include <sstream>
#include <stdexcept>

namespace qb {

// Helper function for OpenSSL error handling
static std::string
get_openssl_asymmetric_error() {
    char          err_buf[256];
    unsigned long err = ERR_get_error();
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    return std::string(err_buf);
}

// Helper to convert EVP_PKEY to string
static std::string
key_to_pem(EVP_PKEY *pkey, bool is_private) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        throw std::runtime_error("Failed to allocate memory for key conversion"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    int result;
    if (is_private) {
        result = PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    } else {
        result = PEM_write_bio_PUBKEY(bio, pkey);
    }

    if (result != 1) {
        BIO_free(bio);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to write key to PEM: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    char       *pem_ptr;
    long        pem_size = BIO_get_mem_data(bio, &pem_ptr);
    std::string pem_str(pem_ptr, pem_size);

    BIO_free(bio);
    return pem_str;
}

// Helper to convert PEM string to EVP_PKEY
static EVP_PKEY *
pem_to_key(const std::string &pem_str, bool is_private) {
    BIO *bio = BIO_new_mem_buf(pem_str.c_str(), -1);
    if (!bio) {
        throw std::runtime_error("Failed to allocate memory for key parsing"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    EVP_PKEY *pkey;
    if (is_private) {
        pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    } else {
        pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    }

    BIO_free(bio);

    if (!pkey) {
        throw std::runtime_error("Failed to parse PEM key: " + get_openssl_asymmetric_error());
    }

    return pkey;
}

// Helper to extract raw key bytes from EVP_PKEY
static std::vector<unsigned char>
get_raw_key_bytes(EVP_PKEY *pkey, bool is_private) {
    size_t key_len;
    if (EVP_PKEY_get_raw_private_key(pkey, NULL, &key_len) != 1 && EVP_PKEY_get_raw_public_key(pkey, NULL, &key_len) != 1) {
        throw std::runtime_error("Failed to determine key length: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> key_bytes(key_len);
    if (is_private) {
        if (EVP_PKEY_get_raw_private_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract private key bytes: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    } else {
        if (EVP_PKEY_get_raw_public_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract public key bytes: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_asymmetric_error());         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
    }

    return key_bytes;
}

// Implementation of RSA key pair generation (PEM format).
// Declared in crypto.h but previously had no definition — any caller failed to
// link. Mirrors generate_ed25519_keypair's EVP keygen + key_to_pem flow.
std::pair<std::string, std::string>
crypto::generate_rsa_keypair(int bits) {
    if (bits < 2048) {
        throw std::runtime_error("RSA key size must be at least 2048 bits");
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) {
        throw std::runtime_error("Failed to create RSA context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize RSA key generation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) != 1) {
        EVP_PKEY_CTX_free(ctx);                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to set RSA key size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("RSA key generation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::string private_key_pem = key_to_pem(pkey, true);
    std::string public_key_pem  = key_to_pem(pkey, false);

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);

    return std::make_pair(private_key_pem, public_key_pem);
}

// Implementation of EC key pair generation (PEM format). Curve is a short name
// such as "prime256v1" (P-256/ES256), "secp384r1", "secp521r1". Declared in
// crypto.h but previously had no definition.
std::pair<std::string, std::string>
crypto::generate_ec_keypair(const std::string &curve) {
    const int nid = OBJ_sn2nid(curve.c_str());
    if (nid == NID_undef) {
        throw std::runtime_error("Unknown EC curve: " + curve);
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!ctx) {
        throw std::runtime_error("Failed to create EC context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize EC key generation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid) != 1) {
        EVP_PKEY_CTX_free(ctx);                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to set EC curve: " +     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("EC key generation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::string private_key_pem = key_to_pem(pkey, true);
    std::string public_key_pem  = key_to_pem(pkey, false);

    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);

    return std::make_pair(private_key_pem, public_key_pem);
}

// Implementation of Ed25519 key pair generation (PEM format)
std::pair<std::string, std::string>
crypto::generate_ed25519_keypair() {
    // Create key context for Ed25519
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!ctx) {
        throw std::runtime_error("Failed to create Ed25519 context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize key generation operation
    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize Ed25519 key generation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Generate the key pair
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Ed25519 key generation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Convert to PEM format
    std::string private_key_pem = key_to_pem(pkey, true);
    std::string public_key_pem  = key_to_pem(pkey, false);

    // Cleanup
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);

    return std::make_pair(private_key_pem, public_key_pem);
}

// Implementation of Ed25519 key pair generation (raw bytes)
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::generate_ed25519_keypair_bytes() {
    // First generate the key pair in PEM format
    auto [private_key_pem, public_key_pem] = generate_ed25519_keypair();

    // Convert private key to EVP_PKEY
    EVP_PKEY *pkey = pem_to_key(private_key_pem, true);

    // Extract raw key bytes
    std::vector<unsigned char> private_key_bytes = get_raw_key_bytes(pkey, true);
    std::vector<unsigned char> public_key_bytes  = get_raw_key_bytes(pkey, false);

    // Cleanup
    EVP_PKEY_free(pkey);

    return std::make_pair(private_key_bytes, public_key_bytes);
}

// Implementation of Ed25519 signing with PEM key
std::vector<unsigned char>
crypto::ed25519_sign(const std::vector<unsigned char> &data, const std::string &private_key_pem) {
    // Parse the private key
    EVP_PKEY *pkey = pem_to_key(private_key_pem, true);

    // Create signing context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine the signature size
    size_t sig_len;
    if (EVP_DigestSign(md_ctx, NULL, &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Create the signature
    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length (which might be smaller than initially
    // allocated)
    signature.resize(sig_len);

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return signature;
}

// Implementation of Ed25519 signing with raw key bytes
std::vector<unsigned char>
crypto::ed25519_sign(const std::vector<unsigned char> &data, const std::vector<unsigned char> &private_key_bytes) {
    // Create key from raw bytes
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, private_key_bytes.data(), private_key_bytes.size());
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " + get_openssl_asymmetric_error());
    }

    // Create signing context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine the signature size
    size_t sig_len;
    if (EVP_DigestSign(md_ctx, NULL, &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());         // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Create the signature
    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length
    signature.resize(sig_len);

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return signature;
}

// Implementation of Ed25519 verification with PEM key
bool
crypto::ed25519_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key_pem) {
    // Parse the public key
    EVP_PKEY *pkey = pem_to_key(public_key_pem, false);

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Verify the signature
    int result = EVP_DigestVerify(md_ctx, signature.data(), signature.size(), data.data(), data.size());

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (result == 1);
}

// Implementation of Ed25519 verification with raw key bytes
bool
crypto::ed25519_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature,
                       const std::vector<unsigned char> &public_key_bytes) {
    // Create key from raw bytes
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key_bytes.data(), public_key_bytes.size());
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " + get_openssl_asymmetric_error());
    }

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Verify the signature
    int result = EVP_DigestVerify(md_ctx, signature.data(), signature.size(), data.data(), data.size());

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (result == 1);
}

// Implementation of X25519 key pair generation (PEM format)
std::pair<std::string, std::string>
crypto::generate_x25519_keypair() {
    // Create key context for X25519
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!ctx) {
        throw std::runtime_error("Failed to create X25519 context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize key generation operation
    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize X25519 key generation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Generate the key pair
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("X25519 key generation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Convert to PEM format
    std::string private_key_pem = key_to_pem(pkey, true);
    std::string public_key_pem  = key_to_pem(pkey, false);

    // Cleanup
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);

    return std::make_pair(private_key_pem, public_key_pem);
}

// Implementation of X25519 key pair generation (raw bytes)
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::generate_x25519_keypair_bytes() {
    // First generate the key pair in PEM format
    auto [private_key_pem, public_key_pem] = generate_x25519_keypair();

    // Convert private key to EVP_PKEY
    EVP_PKEY *pkey = pem_to_key(private_key_pem, true);

    // Extract raw key bytes
    std::vector<unsigned char> private_key_bytes = get_raw_key_bytes(pkey, true);
    std::vector<unsigned char> public_key_bytes  = get_raw_key_bytes(pkey, false);

    // Cleanup
    EVP_PKEY_free(pkey);

    return std::make_pair(private_key_bytes, public_key_bytes);
}

// Implementation of X25519 key exchange with PEM keys
std::vector<unsigned char>
crypto::x25519_key_exchange(const std::string &private_key_pem, const std::string &peer_public_key_pem) {
    // Parse the keys. priv_key is allocated first; if parsing the peer key throws
    // (e.g. an incompatible / malformed PEM), free priv_key before propagating so it
    // does not leak.
    EVP_PKEY *priv_key = pem_to_key(private_key_pem, true);
    EVP_PKEY *pub_key  = nullptr;
    try {
        pub_key = pem_to_key(peer_public_key_pem, false);
    } catch (...) {
        EVP_PKEY_free(priv_key);
        throw;
    }

    // Reject a peer key of the wrong ALGORITHM here, before OpenSSL ever sees it.
    //
    // Both PEMs are caller-supplied, and in a key-agreement handshake the PEER one normally comes
    // off the wire — so its algorithm is chosen by the remote end, not by us. A well-formed
    // Ed25519 public key parses fine above (it is valid PEM, merely the wrong curve) and then
    // reaches EVP_PKEY_derive_set_peer() as a type mismatch, which OpenSSL treats as a CALLER bug
    // rather than an input error: crypto/evp/keymgmt_lib.c asserts
    // `match_type(pk->keymgmt, keymgmt)`. On an OpenSSL built with NDEBUG (the usual release
    // packaging on Linux/macOS) `ossl_assert` degrades to a plain test and the call merely returns
    // 0, so the throw below is reached and nothing looks wrong. On an OpenSSL built WITH
    // assertions (vcpkg's Windows debug triplet, and several distro debug builds) the same call
    // reaches OPENSSL_die() and **abort()s the process** — a remote peer picking the wrong key
    // type takes the server down.
    //
    // Doing the check ourselves makes the rejection qb's own and therefore independent of how the
    // linked OpenSSL happens to be configured.
    //
    // The test is deliberately "the two key types AGREE", not "both are X25519", even though the
    // header documents this as an X25519 helper: agreement is EXACTLY the precondition
    // EVP_PKEY_derive_set_peer() asserts on, so this guard cannot reject any input that used to
    // work (a matched X448 or EC pair still derives, as it did before). It only converts the
    // undefined-behaviour case into a clean exception.
    //
    // The raw-bytes overload below needs no equivalent guard: it builds BOTH keys itself with
    // EVP_PKEY_new_raw_{private,public}_key(EVP_PKEY_X25519, ...), so the types match by
    // construction and a wrong-algorithm peer cannot be expressed.
    if (EVP_PKEY_base_id(priv_key) != EVP_PKEY_base_id(pub_key)) {
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("x25519_key_exchange: peer public key algorithm does not match the private key's");
    }

    // Create key exchange context
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv_key, NULL);
    if (!ctx) {
        EVP_PKEY_free(priv_key);                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create key exchange context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize key derivation
    if (EVP_PKEY_derive_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize key derivation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Set peer key
    if (EVP_PKEY_derive_set_peer(ctx, pub_key) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to set peer key: " + get_openssl_asymmetric_error());
    }

    // Determine buffer length for shared secret
    size_t secret_len;
    if (EVP_PKEY_derive(ctx, NULL, &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine shared secret length: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Derive the shared secret
    std::vector<unsigned char> shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Key derivation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual secret length
    shared_secret.resize(secret_len);

    // Cleanup
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv_key);
    EVP_PKEY_free(pub_key);

    return shared_secret;
}

// Implementation of X25519 key exchange with raw key bytes
std::vector<unsigned char>
crypto::x25519_key_exchange(const std::vector<unsigned char> &private_key_bytes, const std::vector<unsigned char> &peer_public_key_bytes) {
    // Create keys from raw bytes
    EVP_PKEY *priv_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key_bytes.data(), private_key_bytes.size());
    if (!priv_key) {
        throw std::runtime_error("Failed to create private key from raw bytes: " + get_openssl_asymmetric_error());
    }

    EVP_PKEY *pub_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key_bytes.data(), peer_public_key_bytes.size());
    if (!pub_key) {
        EVP_PKEY_free(priv_key);
        throw std::runtime_error("Failed to create public key from raw bytes: " + get_openssl_asymmetric_error());
    }

    // Create key exchange context
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv_key, NULL);
    if (!ctx) {
        EVP_PKEY_free(priv_key);                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create key exchange context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize key derivation
    if (EVP_PKEY_derive_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize key derivation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Set peer key
    if (EVP_PKEY_derive_set_peer(ctx, pub_key) != 1) {
        EVP_PKEY_CTX_free(ctx);                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                   // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to set peer key: " +     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine buffer length for shared secret
    size_t secret_len;
    if (EVP_PKEY_derive(ctx, NULL, &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine shared secret length: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Derive the shared secret
    std::vector<unsigned char> shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(priv_key);                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pub_key);                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Key derivation failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual secret length
    shared_secret.resize(secret_len);

    // Cleanup
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(priv_key);
    EVP_PKEY_free(pub_key);

    return shared_secret;
}

// Implementation of ECIES encryption with raw key bytes
std::pair<std::vector<unsigned char>, std::vector<unsigned char>>
crypto::ecies_encrypt(const std::vector<unsigned char> &data, const std::vector<unsigned char> &recipient_public_key,
                      const std::vector<unsigned char> &optional_shared_info, ECIESMode mode) {
    // Generate ephemeral X25519 key pair
    auto [ephemeral_priv_key, ephemeral_pub_key] = generate_x25519_keypair_bytes();

    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret = x25519_key_exchange(ephemeral_priv_key, recipient_public_key);

    // Derive encryption key and IV using HKDF
    std::vector<unsigned char> key_material = hkdf(shared_secret,
                                                   optional_shared_info, // Use shared info as salt
                                                   {},                   // Empty info
                                                   64,                   // 32 bytes for key, 16 for IV
                                                   DigestAlgorithm::SHA256);

    // Select symmetric algorithm + nonce length from the mode. GCM/ChaCha take a
    // 12-byte nonce, CBC a 16-byte IV. Slicing the exact length (not a fixed 16)
    // keeps AEAD nonces at the standard 96 bits now that the symmetric path
    // requires an exact IV length. This is byte-identical to the previous
    // behaviour: OpenSSL already consumed only the first 12 bytes of the old
    // 16-byte GCM/ChaCha IV, so the derived nonce is unchanged.
    SymmetricAlgorithm sym_algorithm;
    std::size_t        iv_len;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            iv_len        = 12;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            iv_len        = 12;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            iv_len        = 16;
            break;
    }

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(), key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 32 + iv_len);

    // Encrypt the data using the derived key and IV
    std::vector<unsigned char> encrypted_data = encrypt(data, symmetric_key, iv, sym_algorithm);

    // Return ephemeral public key and encrypted data
    return std::make_pair(ephemeral_pub_key, encrypted_data);
}

// Implementation of ECIES decryption with raw key bytes
std::vector<unsigned char>
crypto::ecies_decrypt(const std::vector<unsigned char> &encrypted_data, const std::vector<unsigned char> &ephemeral_public_key,
                      const std::vector<unsigned char> &recipient_private_key, const std::vector<unsigned char> &optional_shared_info,
                      ECIESMode mode) {
    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret = x25519_key_exchange(recipient_private_key, ephemeral_public_key);

    // Derive decryption key and IV using HKDF
    std::vector<unsigned char> key_material = hkdf(shared_secret,
                                                   optional_shared_info, // Use shared info as salt
                                                   {},                   // Empty info
                                                   64,                   // 32 bytes for key, 16 for IV
                                                   DigestAlgorithm::SHA256);

    // Select symmetric algorithm + nonce length from the mode (must mirror
    // ecies_encrypt): GCM/ChaCha 12-byte nonce, CBC 16-byte IV. Byte-identical
    // to the prior fixed-16 slice for the derived nonce (see ecies_encrypt).
    SymmetricAlgorithm sym_algorithm;
    std::size_t        iv_len;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            iv_len        = 12;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            iv_len        = 12;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            iv_len        = 16;
            break;
    }

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(), key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 32 + iv_len);

    // Decrypt the data using the derived key and IV
    std::vector<unsigned char> decrypted_data = decrypt(encrypted_data, symmetric_key, iv, sym_algorithm);

    return decrypted_data;
}

// Implementation of RSA signature
std::vector<unsigned char>
crypto::rsa_sign(const std::vector<unsigned char> &data, const std::string &private_key, DigestAlgorithm digest) {
    // Parse the private key from PEM format
    EVP_PKEY *pkey = pem_to_key(private_key, true);

    // Get the EVP_MD for the digest algorithm
    const EVP_MD *md = get_evp_md(digest);
    if (!md) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Invalid digest algorithm for RSA signing");
    }

    // Create signature context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize RSA signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Update the context with the data to be signed
    if (EVP_DigestSignUpdate(md_ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to update RSA signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine the signature size
    size_t sig_len = 0;
    if (EVP_DigestSignFinal(md_ctx, nullptr, &sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx);                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine RSA signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Allocate memory for the signature
    std::vector<unsigned char> signature(sig_len);

    // Get the signature
    if (EVP_DigestSignFinal(md_ctx, signature.data(), &sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("RSA signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length
    signature.resize(sig_len);

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return signature;
}

// Implementation of RSA verification
bool
crypto::rsa_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key,
                   DigestAlgorithm digest) {
    // Parse the public key from PEM format
    EVP_PKEY *pkey = pem_to_key(public_key, false);

    // Get the EVP_MD for the digest algorithm
    const EVP_MD *md = get_evp_md(digest);
    if (!md) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Invalid digest algorithm for RSA verification");
    }

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize RSA verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Update the context with the data to be verified
    if (EVP_DigestVerifyUpdate(md_ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to update RSA verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Verify the signature
    int result = EVP_DigestVerifyFinal(md_ctx, signature.data(), signature.size());

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (result == 1);
}

// Implementation of EC signature
std::vector<unsigned char>
crypto::ec_sign(const std::vector<unsigned char> &data, const std::string &private_key, DigestAlgorithm digest) {
    // Parse the private key from PEM format
    EVP_PKEY *pkey = pem_to_key(private_key, true);

    // Get the EVP_MD for the digest algorithm
    const EVP_MD *md = get_evp_md(digest);
    if (!md) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Invalid digest algorithm for EC signing");
    }

    // Create signature context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize EC signing operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Update the context with the data to be signed
    if (EVP_DigestSignUpdate(md_ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to update EC signing context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Determine the signature size
    size_t sig_len = 0;
    if (EVP_DigestSignFinal(md_ctx, nullptr, &sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx);                                             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to determine EC signature size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Allocate memory for the signature
    std::vector<unsigned char> signature(sig_len);

    // Get the signature
    if (EVP_DigestSignFinal(md_ctx, signature.data(), &sig_len) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                              // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("EC signing failed: " + get_openssl_asymmetric_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Resize to actual signature length
    signature.resize(sig_len);

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return signature;
}

// Implementation of EC verification
bool
crypto::ec_verify(const std::vector<unsigned char> &data, const std::vector<unsigned char> &signature, const std::string &public_key,
                  DigestAlgorithm digest) {
    // Parse the public key from PEM format
    EVP_PKEY *pkey = pem_to_key(public_key, false);

    // Get the EVP_MD for the digest algorithm
    const EVP_MD *md = get_evp_md(digest);
    if (!md) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Invalid digest algorithm for EC verification");
    }

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);                                                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to create verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());            // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                      // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                          // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to initialize EC verification operation: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());                     // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Update the context with the data to be verified
    if (EVP_DigestVerifyUpdate(md_ctx, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);                                                // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        EVP_PKEY_free(pkey);                                                    // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        throw std::runtime_error("Failed to update EC verification context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_asymmetric_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Verify the signature
    int result = EVP_DigestVerifyFinal(md_ctx, signature.data(), signature.size());

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (result == 1);
}

} // namespace qb
