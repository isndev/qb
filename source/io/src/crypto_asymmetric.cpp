/**
 * @file qb/source/io/src/crypto_asymmetric.cpp
 * @brief Implementation of asymmetric cryptographic utilities for the QB IO library
 *
 * This file provides implementations of modern asymmetric cryptographic operations such
 * as:
 * - Ed25519 for digital signatures
 * - X25519 for key exchange
 * - ECIES (Elliptic Curve Integrated Encryption Scheme) for hybrid encryption
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

#include <cstring>
#include <fstream>
#include <memory>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
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
        throw std::runtime_error("Failed to allocate memory for key conversion");
    }

    int result;
    if (is_private) {
        result = PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    } else {
        result = PEM_write_bio_PUBKEY(bio, pkey);
    }

    if (result != 1) {
        BIO_free(bio);
        throw std::runtime_error("Failed to write key to PEM: " +
                                 get_openssl_asymmetric_error());
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
        throw std::runtime_error("Failed to allocate memory for key parsing");
    }

    EVP_PKEY *pkey;
    if (is_private) {
        pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    } else {
        pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    }

    BIO_free(bio);

    if (!pkey) {
        throw std::runtime_error("Failed to parse PEM key: " +
                                 get_openssl_asymmetric_error());
    }

    return pkey;
}

// Helper to extract raw key bytes from EVP_PKEY
static std::vector<unsigned char>
get_raw_key_bytes(EVP_PKEY *pkey, bool is_private) {
    size_t key_len;
    if (EVP_PKEY_get_raw_private_key(pkey, NULL, &key_len) != 1 &&
        EVP_PKEY_get_raw_public_key(pkey, NULL, &key_len) != 1) {
        throw std::runtime_error("Failed to determine key length: " +
                                 get_openssl_asymmetric_error());
    }

    std::vector<unsigned char> key_bytes(key_len);
    if (is_private) {
        if (EVP_PKEY_get_raw_private_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract private key bytes: " +
                                     get_openssl_asymmetric_error());
        }
    } else {
        if (EVP_PKEY_get_raw_public_key(pkey, key_bytes.data(), &key_len) != 1) {
            throw std::runtime_error("Failed to extract public key bytes: " +
                                     get_openssl_asymmetric_error());
        }
    }

    return key_bytes;
}

// Implementation of Ed25519 key pair generation (PEM format)
std::pair<std::string, std::string>
crypto::generate_ed25519_keypair() {
    // Create key context for Ed25519
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!ctx) {
        throw std::runtime_error("Failed to create Ed25519 context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize key generation operation
    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize Ed25519 key generation: " +
                                 get_openssl_asymmetric_error());
    }

    // Generate the key pair
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Ed25519 key generation failed: " +
                                 get_openssl_asymmetric_error());
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
crypto::ed25519_sign(const std::vector<unsigned char> &data,
                     const std::string                &private_key_pem) {
    // Parse the private key
    EVP_PKEY *pkey = pem_to_key(private_key_pem, true);

    // Create signing context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create signing context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to initialize signing operation: " +
                                 get_openssl_asymmetric_error());
    }

    // Determine the signature size
    size_t sig_len;
    if (EVP_DigestSign(md_ctx, NULL, &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to determine signature size: " +
                                 get_openssl_asymmetric_error());
    }

    // Create the signature
    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &sig_len, data.data(), data.size()) !=
        1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Signing failed: " + get_openssl_asymmetric_error());
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
crypto::ed25519_sign(const std::vector<unsigned char> &data,
                     const std::vector<unsigned char> &private_key_bytes) {
    // Create key from raw bytes
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, NULL, private_key_bytes.data(), private_key_bytes.size());
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " +
                                 get_openssl_asymmetric_error());
    }

    // Create signing context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create signing context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize the signing operation
    if (EVP_DigestSignInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to initialize signing operation: " +
                                 get_openssl_asymmetric_error());
    }

    // Determine the signature size
    size_t sig_len;
    if (EVP_DigestSign(md_ctx, NULL, &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to determine signature size: " +
                                 get_openssl_asymmetric_error());
    }

    // Create the signature
    std::vector<unsigned char> signature(sig_len);
    if (EVP_DigestSign(md_ctx, signature.data(), &sig_len, data.data(), data.size()) !=
        1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Signing failed: " + get_openssl_asymmetric_error());
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
crypto::ed25519_verify(const std::vector<unsigned char> &data,
                       const std::vector<unsigned char> &signature,
                       const std::string                &public_key_pem) {
    // Parse the public key
    EVP_PKEY *pkey = pem_to_key(public_key_pem, false);

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create verification context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to initialize verification operation: " +
                                 get_openssl_asymmetric_error());
    }

    // Verify the signature
    int result = EVP_DigestVerify(md_ctx, signature.data(), signature.size(),
                                  data.data(), data.size());

    // Cleanup
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return (result == 1);
}

// Implementation of Ed25519 verification with raw key bytes
bool
crypto::ed25519_verify(const std::vector<unsigned char> &data,
                       const std::vector<unsigned char> &signature,
                       const std::vector<unsigned char> &public_key_bytes) {
    // Create key from raw bytes
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, NULL, public_key_bytes.data(), public_key_bytes.size());
    if (!pkey) {
        throw std::runtime_error("Failed to create key from raw bytes: " +
                                 get_openssl_asymmetric_error());
    }

    // Create verification context
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to create verification context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize the verification operation
    if (EVP_DigestVerifyInit(md_ctx, NULL, NULL, NULL, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to initialize verification operation: " +
                                 get_openssl_asymmetric_error());
    }

    // Verify the signature
    int result = EVP_DigestVerify(md_ctx, signature.data(), signature.size(),
                                  data.data(), data.size());

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
        throw std::runtime_error("Failed to create X25519 context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize key generation operation
    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize X25519 key generation: " +
                                 get_openssl_asymmetric_error());
    }

    // Generate the key pair
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("X25519 key generation failed: " +
                                 get_openssl_asymmetric_error());
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
crypto::x25519_key_exchange(const std::string &private_key_pem,
                            const std::string &peer_public_key_pem) {
    // Parse the keys
    EVP_PKEY *priv_key = pem_to_key(private_key_pem, true);
    EVP_PKEY *pub_key  = pem_to_key(peer_public_key_pem, false);

    // Create key exchange context
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv_key, NULL);
    if (!ctx) {
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to create key exchange context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize key derivation
    if (EVP_PKEY_derive_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to initialize key derivation: " +
                                 get_openssl_asymmetric_error());
    }

    // Set peer key
    if (EVP_PKEY_derive_set_peer(ctx, pub_key) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to set peer key: " +
                                 get_openssl_asymmetric_error());
    }

    // Determine buffer length for shared secret
    size_t secret_len;
    if (EVP_PKEY_derive(ctx, NULL, &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to determine shared secret length: " +
                                 get_openssl_asymmetric_error());
    }

    // Derive the shared secret
    std::vector<unsigned char> shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Key derivation failed: " +
                                 get_openssl_asymmetric_error());
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
crypto::x25519_key_exchange(const std::vector<unsigned char> &private_key_bytes,
                            const std::vector<unsigned char> &peer_public_key_bytes) {
    // Create keys from raw bytes
    EVP_PKEY *priv_key = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, NULL, private_key_bytes.data(), private_key_bytes.size());
    if (!priv_key) {
        throw std::runtime_error("Failed to create private key from raw bytes: " +
                                 get_openssl_asymmetric_error());
    }

    EVP_PKEY *pub_key =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key_bytes.data(),
                                    peer_public_key_bytes.size());
    if (!pub_key) {
        EVP_PKEY_free(priv_key);
        throw std::runtime_error("Failed to create public key from raw bytes: " +
                                 get_openssl_asymmetric_error());
    }

    // Create key exchange context
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(priv_key, NULL);
    if (!ctx) {
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to create key exchange context: " +
                                 get_openssl_asymmetric_error());
    }

    // Initialize key derivation
    if (EVP_PKEY_derive_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to initialize key derivation: " +
                                 get_openssl_asymmetric_error());
    }

    // Set peer key
    if (EVP_PKEY_derive_set_peer(ctx, pub_key) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to set peer key: " +
                                 get_openssl_asymmetric_error());
    }

    // Determine buffer length for shared secret
    size_t secret_len;
    if (EVP_PKEY_derive(ctx, NULL, &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Failed to determine shared secret length: " +
                                 get_openssl_asymmetric_error());
    }

    // Derive the shared secret
    std::vector<unsigned char> shared_secret(secret_len);
    if (EVP_PKEY_derive(ctx, shared_secret.data(), &secret_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(priv_key);
        EVP_PKEY_free(pub_key);
        throw std::runtime_error("Key derivation failed: " +
                                 get_openssl_asymmetric_error());
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
crypto::ecies_encrypt(const std::vector<unsigned char> &data,
                      const std::vector<unsigned char> &recipient_public_key,
                      const std::vector<unsigned char> &optional_shared_info,
                      ECIESMode                         mode) {
    // Generate ephemeral X25519 key pair
    auto [ephemeral_priv_key, ephemeral_pub_key] = generate_x25519_keypair_bytes();

    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret =
        x25519_key_exchange(ephemeral_priv_key, recipient_public_key);

    // Derive encryption key and IV using HKDF
    std::vector<unsigned char> key_material =
        hkdf(shared_secret,
             optional_shared_info, // Use shared info as salt
             {},                   // Empty info
             64,                   // 32 bytes for key, 16 for IV
             DigestAlgorithm::SHA256);

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(),
                                             key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 48);

    // Select symmetric algorithm based on mode
    SymmetricAlgorithm sym_algorithm;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            break;
    }

    // Encrypt the data using the derived key and IV
    std::vector<unsigned char> encrypted_data =
        encrypt(data, symmetric_key, iv, sym_algorithm);

    // Return ephemeral public key and encrypted data
    return std::make_pair(ephemeral_pub_key, encrypted_data);
}

// Implementation of ECIES decryption with raw key bytes
std::vector<unsigned char>
crypto::ecies_decrypt(const std::vector<unsigned char> &encrypted_data,
                      const std::vector<unsigned char> &ephemeral_public_key,
                      const std::vector<unsigned char> &recipient_private_key,
                      const std::vector<unsigned char> &optional_shared_info,
                      ECIESMode                         mode) {
    // Perform X25519 key exchange to derive shared secret
    std::vector<unsigned char> shared_secret =
        x25519_key_exchange(recipient_private_key, ephemeral_public_key);

    // Derive decryption key and IV using HKDF
    std::vector<unsigned char> key_material =
        hkdf(shared_secret,
             optional_shared_info, // Use shared info as salt
             {},                   // Empty info
             64,                   // 32 bytes for key, 16 for IV
             DigestAlgorithm::SHA256);

    // Extract key and IV from the derived material
    std::vector<unsigned char> symmetric_key(key_material.begin(),
                                             key_material.begin() + 32);
    std::vector<unsigned char> iv(key_material.begin() + 32, key_material.begin() + 48);

    // Select symmetric algorithm based on mode
    SymmetricAlgorithm sym_algorithm;
    switch (mode) {
        case ECIESMode::AES_GCM:
            sym_algorithm = SymmetricAlgorithm::AES_256_GCM;
            break;
        case ECIESMode::CHACHA20:
            sym_algorithm = SymmetricAlgorithm::CHACHA20_POLY1305;
            break;
        case ECIESMode::STANDARD:
        default:
            sym_algorithm = SymmetricAlgorithm::AES_256_CBC;
            break;
    }

    // Decrypt the data using the derived key and IV
    std::vector<unsigned char> decrypted_data =
        decrypt(encrypted_data, symmetric_key, iv, sym_algorithm);

    return decrypted_data;
}

} // namespace qb