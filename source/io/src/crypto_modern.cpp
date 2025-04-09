/**
 * @file qb/io/src/crypto_modern.cpp
 * @brief Implementation of modern cryptographic utilities
 *
 * This file contains the implementation of modern cryptographic functions
 * including symmetric encryption/decryption, key generation, digital signatures,
 * and secure random number generation. It provides a comprehensive set of
 * cryptographic utilities for the QB framework using the latest OpenSSL APIs.
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

#include <memory>
#include <openssl/err.h>
#include <qb/io/crypto.h>

namespace qb {

// Helper for OpenSSL error handling
static std::string
get_openssl_error() {
    char          err_buf[256];
    unsigned long err = ERR_get_error();
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    return std::string(err_buf);
}

// Convert algorithm enum to EVP_MD
const EVP_MD *
crypto::get_evp_md(DigestAlgorithm algorithm) {
    switch (algorithm) {
        case DigestAlgorithm::MD5:
            return EVP_md5();
        case DigestAlgorithm::SHA1:
            return EVP_sha1();
        case DigestAlgorithm::SHA224:
            return EVP_sha224();
        case DigestAlgorithm::SHA256:
            return EVP_sha256();
        case DigestAlgorithm::SHA384:
            return EVP_sha384();
        case DigestAlgorithm::SHA512:
            return EVP_sha512();
        case DigestAlgorithm::BLAKE2B512:
            return EVP_blake2b512();
        case DigestAlgorithm::BLAKE2S256:
            return EVP_blake2s256();
        default:
            return nullptr;
    }
}

// Generate cryptographically secure random bytes
std::vector<unsigned char>
crypto::generate_random_bytes(size_t size) {
    std::vector<unsigned char> bytes(size);
    if (!secure_random_fill(bytes)) {
        throw std::runtime_error("Failed to generate random bytes: " +
                                 get_openssl_error());
    }
    return bytes;
}

bool
crypto::secure_random_fill(std::vector<unsigned char> &buffer) {
    return RAND_bytes(buffer.data(), buffer.size()) == 1;
}

// Generate initialization vector (IV)
std::vector<unsigned char>
crypto::generate_iv(SymmetricAlgorithm algorithm) {
    size_t iv_size = 0;

    switch (algorithm) {
        case SymmetricAlgorithm::AES_128_CBC:
        case SymmetricAlgorithm::AES_192_CBC:
        case SymmetricAlgorithm::AES_256_CBC:
            iv_size = 16; // AES block size
            break;
        case SymmetricAlgorithm::AES_128_GCM:
        case SymmetricAlgorithm::AES_192_GCM:
        case SymmetricAlgorithm::AES_256_GCM:
            iv_size = 12; // Recommended for GCM
            break;
        case SymmetricAlgorithm::CHACHA20_POLY1305:
            iv_size = 12; // ChaCha20-Poly1305 nonce size
            break;
        default:
            throw std::runtime_error("Unknown symmetric algorithm");
    }

    return generate_random_bytes(iv_size);
}

// Generate encryption key
std::vector<unsigned char>
crypto::generate_key(SymmetricAlgorithm algorithm) {
    size_t key_size = 0;

    switch (algorithm) {
        case SymmetricAlgorithm::AES_128_CBC:
        case SymmetricAlgorithm::AES_128_GCM:
            key_size = 16; // 128 bits
            break;
        case SymmetricAlgorithm::AES_192_CBC:
        case SymmetricAlgorithm::AES_192_GCM:
            key_size = 24; // 192 bits
            break;
        case SymmetricAlgorithm::AES_256_CBC:
        case SymmetricAlgorithm::AES_256_GCM:
        case SymmetricAlgorithm::CHACHA20_POLY1305:
            key_size = 32; // 256 bits
            break;
        default:
            throw std::runtime_error("Unknown symmetric algorithm");
    }

    return generate_random_bytes(key_size);
}

// Symmetric encryption implementation
std::vector<unsigned char>
crypto::encrypt(const std::vector<unsigned char> &plaintext,
                const std::vector<unsigned char> &key,
                const std::vector<unsigned char> &iv, SymmetricAlgorithm algorithm,
                const std::vector<unsigned char> &aad) {
    const EVP_CIPHER *cipher  = nullptr;
    int               tag_len = 0;
    bool              is_aead = false;

    // Select appropriate cipher
    switch (algorithm) {
        case SymmetricAlgorithm::AES_128_CBC:
            cipher = EVP_aes_128_cbc();
            break;
        case SymmetricAlgorithm::AES_192_CBC:
            cipher = EVP_aes_192_cbc();
            break;
        case SymmetricAlgorithm::AES_256_CBC:
            cipher = EVP_aes_256_cbc();
            break;
        case SymmetricAlgorithm::AES_128_GCM:
            cipher  = EVP_aes_128_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::AES_192_GCM:
            cipher  = EVP_aes_192_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::AES_256_GCM:
            cipher  = EVP_aes_256_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::CHACHA20_POLY1305:
            cipher  = EVP_chacha20_poly1305();
            tag_len = 16;
            is_aead = true;
            break;
        default:
            throw std::runtime_error("Unknown symmetric algorithm");
    }

    // Create context
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context: " +
                                 get_openssl_error());
    }

    // Pre-allocate output buffer with space for ciphertext and tag (if AEAD)
    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH +
                                          tag_len);
    int                        out_len   = 0;
    int                        final_len = 0;

    try {
        // Initialize encryption operation
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data()) != 1) {
            throw std::runtime_error("Failed to initialize encryption: " +
                                     get_openssl_error());
        }

        // Process AAD for AEAD ciphers
        if (is_aead && !aad.empty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &out_len, aad.data(), aad.size()) != 1) {
                throw std::runtime_error("Failed to process AAD: " +
                                         get_openssl_error());
            }
        }

        // Encrypt plaintext
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len, plaintext.data(),
                              plaintext.size()) != 1) {
            throw std::runtime_error("Failed to encrypt data: " + get_openssl_error());
        }

        int total_len = out_len;

        // Finalize encryption
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &final_len) != 1) {
            throw std::runtime_error("Failed to finalize encryption: " +
                                     get_openssl_error());
        }

        total_len += final_len;

        // For AEAD modes, get the authentication tag
        if (is_aead) {
            std::vector<unsigned char> tag(tag_len);
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag_len, tag.data()) !=
                1) {
                throw std::runtime_error("Failed to get authentication tag: " +
                                         get_openssl_error());
            }

            // Append tag to ciphertext
            ciphertext.resize(total_len);
            ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
        } else {
            ciphertext.resize(total_len);
        }

        EVP_CIPHER_CTX_free(ctx);
        return ciphertext;
    } catch (const std::exception &e) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }
}

// Symmetric decryption implementation
std::vector<unsigned char>
crypto::decrypt(const std::vector<unsigned char> &ciphertext,
                const std::vector<unsigned char> &key,
                const std::vector<unsigned char> &iv, SymmetricAlgorithm algorithm,
                const std::vector<unsigned char> &aad) {
    const EVP_CIPHER *cipher  = nullptr;
    int               tag_len = 0;
    bool              is_aead = false;

    // Select appropriate cipher
    switch (algorithm) {
        case SymmetricAlgorithm::AES_128_CBC:
            cipher = EVP_aes_128_cbc();
            break;
        case SymmetricAlgorithm::AES_192_CBC:
            cipher = EVP_aes_192_cbc();
            break;
        case SymmetricAlgorithm::AES_256_CBC:
            cipher = EVP_aes_256_cbc();
            break;
        case SymmetricAlgorithm::AES_128_GCM:
            cipher  = EVP_aes_128_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::AES_192_GCM:
            cipher  = EVP_aes_192_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::AES_256_GCM:
            cipher  = EVP_aes_256_gcm();
            tag_len = 16;
            is_aead = true;
            break;
        case SymmetricAlgorithm::CHACHA20_POLY1305:
            cipher  = EVP_chacha20_poly1305();
            tag_len = 16;
            is_aead = true;
            break;
        default:
            throw std::runtime_error("Unknown symmetric algorithm");
    }

    // Check if there's enough data for ciphertext + tag
    if (is_aead && ciphertext.size() < static_cast<size_t>(tag_len)) {
        throw std::runtime_error("Ciphertext too short for AEAD mode");
    }

    // Create context
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context: " +
                                 get_openssl_error());
    }

    // Separate the tag from ciphertext for AEAD modes
    std::vector<unsigned char> actual_ciphertext = ciphertext;
    std::vector<unsigned char> tag;

    if (is_aead) {
        size_t ciphertext_len = ciphertext.size() - tag_len;
        tag.assign(ciphertext.begin() + ciphertext_len, ciphertext.end());
        actual_ciphertext.resize(ciphertext_len);
    }

    // Pre-allocate output buffer
    std::vector<unsigned char> plaintext(actual_ciphertext.size() +
                                         EVP_MAX_BLOCK_LENGTH);
    int                        out_len   = 0;
    int                        final_len = 0;

    try {
        // Initialize decryption operation
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data()) != 1) {
            throw std::runtime_error("Failed to initialize decryption: " +
                                     get_openssl_error());
        }

        // Process AAD for AEAD ciphers
        if (is_aead && !aad.empty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &out_len, aad.data(), aad.size()) != 1) {
                throw std::runtime_error("Failed to process AAD: " +
                                         get_openssl_error());
            }
        }

        // For AEAD modes, set the authentication tag
        if (is_aead) {
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag_len, tag.data()) !=
                1) {
                throw std::runtime_error("Failed to set authentication tag: " +
                                         get_openssl_error());
            }
        }

        // Decrypt ciphertext
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, actual_ciphertext.data(),
                              actual_ciphertext.size()) != 1) {
            throw std::runtime_error("Failed to decrypt data: " + get_openssl_error());
        }

        int total_len = out_len;

        // Finalize decryption
        // For AEAD modes, this will also verify the authentication tag
        int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len);
        if (ret != 1) {
            // Authentication failed or padding error
            if (is_aead) {
                return std::vector<unsigned char>(); // Return empty vector on auth
                                                     // failure
            } else {
                throw std::runtime_error("Failed to finalize decryption: " +
                                         get_openssl_error());
            }
        }

        total_len += final_len;
        plaintext.resize(total_len);

        EVP_CIPHER_CTX_free(ctx);
        return plaintext;
    } catch (const std::exception &e) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }
}

// Generic hash implementation
std::vector<unsigned char>
crypto::hash(const std::vector<unsigned char> &data, DigestAlgorithm algorithm) {
    const EVP_MD *md = get_evp_md(algorithm);
    if (!md) {
        throw std::runtime_error("Unknown digest algorithm");
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create digest context: " +
                                 get_openssl_error());
    }

    try {
        if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
            throw std::runtime_error("Failed to initialize digest: " +
                                     get_openssl_error());
        }

        if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
            throw std::runtime_error("Failed to update digest: " + get_openssl_error());
        }

        std::vector<unsigned char> digest(EVP_MD_size(md));
        unsigned int               digest_len = 0;

        if (EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
            throw std::runtime_error("Failed to finalize digest: " +
                                     get_openssl_error());
        }

        digest.resize(digest_len);
        EVP_MD_CTX_free(ctx);
        return digest;
    } catch (const std::exception &e) {
        EVP_MD_CTX_free(ctx);
        throw;
    }
}

// HMAC implementation
std::vector<unsigned char>
crypto::hmac(const std::vector<unsigned char> &data,
             const std::vector<unsigned char> &key, DigestAlgorithm algorithm) {
    const EVP_MD *md = get_evp_md(algorithm);
    if (!md) {
        throw std::runtime_error("Unknown digest algorithm");
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        throw std::runtime_error("Failed to create digest context: " +
                                 get_openssl_error());
    }

    try {
        // Créer la clé pour HMAC
        EVP_PKEY *pkey =
            EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, nullptr, key.data(), key.size());
        if (!pkey) {
            EVP_MD_CTX_free(mdctx);
            throw std::runtime_error("Failed to create HMAC key: " +
                                     get_openssl_error());
        }

        // Initialiser l'opération de signature
        if (EVP_DigestSignInit(mdctx, nullptr, md, nullptr, pkey) != 1) {
            EVP_PKEY_free(pkey);
            EVP_MD_CTX_free(mdctx);
            throw std::runtime_error("Failed to initialize HMAC: " +
                                     get_openssl_error());
        }

        // Mettre à jour le contexte avec les données
        if (EVP_DigestSignUpdate(mdctx, data.data(), data.size()) != 1) {
            EVP_PKEY_free(pkey);
            EVP_MD_CTX_free(mdctx);
            throw std::runtime_error("Failed to update HMAC: " + get_openssl_error());
        }

        // Déterminer la taille du résultat
        size_t hmac_len = 0;
        if (EVP_DigestSignFinal(mdctx, nullptr, &hmac_len) != 1) {
            EVP_PKEY_free(pkey);
            EVP_MD_CTX_free(mdctx);
            throw std::runtime_error("Failed to determine HMAC size: " +
                                     get_openssl_error());
        }

        // Récupérer le résultat final
        std::vector<unsigned char> hmac_value(hmac_len);
        if (EVP_DigestSignFinal(mdctx, hmac_value.data(), &hmac_len) != 1) {
            EVP_PKEY_free(pkey);
            EVP_MD_CTX_free(mdctx);
            throw std::runtime_error("Failed to finalize HMAC: " + get_openssl_error());
        }

        // Nettoyer
        EVP_PKEY_free(pkey);
        EVP_MD_CTX_free(mdctx);
        return hmac_value;
    } catch (const std::exception &e) {
        EVP_MD_CTX_free(mdctx);
        throw;
    }
}

} // namespace qb