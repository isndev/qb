/**
 * @file qb/io/crypto_modern.cpp
 * @brief Implementation of modern cryptographic utilities
 *
 * This file contains the implementation of modern cryptographic functions
 * including symmetric encryption/decryption, key generation, digital signatures,
 * and secure random number generation. It provides a comprehensive set of
 * cryptographic utilities for the QB framework using the latest OpenSSL APIs.
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

static void
validate_symmetric_parameters(const EVP_CIPHER *cipher, const std::vector<unsigned char> &key, const std::vector<unsigned char> &iv,
                              bool is_aead) {
    const auto expected_key_size = static_cast<size_t>(EVP_CIPHER_key_length(cipher));
    const auto expected_iv_size  = static_cast<size_t>(EVP_CIPHER_iv_length(cipher));

    if (key.size() != expected_key_size) {
        throw std::invalid_argument("Invalid key size for symmetric algorithm");
    }

    if (expected_iv_size == 0) {
        return;
    }

    // Require the EXACT IV/nonce length — for AEAD too. The old `iv.size() <
    // expected` accepted an oversized nonce, but the cipher is initialised with
    // only the default (12-byte) length and never SET_IVLEN, so the extra bytes
    // were silently ignored: a caller varying only the tail of a 16-byte "nonce"
    // reused the same effective 96-bit GCM nonce (catastrophic key/tag recovery).
    if (iv.size() != expected_iv_size) {
        throw std::invalid_argument("Invalid IV size for symmetric algorithm");
    }
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
        throw std::runtime_error("Failed to generate random bytes: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    return bytes;
}

bool
crypto::secure_random_fill(std::vector<unsigned char> &buffer) {
    // RAND_bytes takes an int: a chunk at a time, or a buffer past 4 GiB was filled modulo 2^32 and the
    // rest left zero while the call reported success (Huly QB-344).
    return detail::for_each_openssl_chunk(
        buffer.size(), [&buffer](std::size_t offset, int length) { return RAND_bytes(buffer.data() + offset, length) == 1; });
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
crypto::encrypt(const std::vector<unsigned char> &plaintext, const std::vector<unsigned char> &key, const std::vector<unsigned char> &iv,
                SymmetricAlgorithm algorithm, const std::vector<unsigned char> &aad) {
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

    validate_symmetric_parameters(cipher, key, iv, is_aead);

    // Owned the line it is created: the output allocation below may throw (Huly QB-345).
    const detail::openssl_ptr<EVP_CIPHER_CTX> ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Room for the ciphertext (a block mode adds at most one block of padding) and the tag (if AEAD).
    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH + tag_len);
    std::size_t                total = 0;

    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, key.data(), iv.data()) != 1) {
        throw std::runtime_error("Failed to initialize encryption: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // The AAD, then the plaintext, through EVP_EncryptUpdate's int length a chunk at a time (Huly
    // QB-973): handed over whole, 4 GiB + k bytes wrapped to k and the call encrypted only those, with a
    // valid tag.
    if (is_aead && !aad.empty() && !detail::for_each_openssl_chunk(aad.size(), [&](std::size_t offset, int length) {
            int ignored = 0;
            return EVP_EncryptUpdate(ctx.get(), nullptr, &ignored, aad.data() + offset, length) == 1;
        })) {
        throw std::runtime_error("Failed to process AAD: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    if (!detail::for_each_openssl_chunk(plaintext.size(), [&](std::size_t offset, int length) {
            int written = 0;
            if (EVP_EncryptUpdate(ctx.get(), ciphertext.data() + total, &written, plaintext.data() + offset, length) != 1)
                return false;
            total += static_cast<std::size_t>(written);
            return true;
        })) {
        throw std::runtime_error("Failed to encrypt data: " + get_openssl_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + total, &final_len) != 1) {
        throw std::runtime_error("Failed to finalize encryption: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    total += static_cast<std::size_t>(final_len);

    // For AEAD modes, the tag goes straight after the ciphertext, in the room reserved above.
    if (is_aead) {
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag_len, ciphertext.data() + total) != 1) {
            throw std::runtime_error("Failed to get authentication tag: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                     get_openssl_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        }
        total += static_cast<std::size_t>(tag_len);
    }

    ciphertext.resize(total);
    return ciphertext;
}

// Symmetric decryption implementation
std::vector<unsigned char>
crypto::decrypt(const std::vector<unsigned char> &ciphertext, const std::vector<unsigned char> &key, const std::vector<unsigned char> &iv,
                SymmetricAlgorithm algorithm, const std::vector<unsigned char> &aad) {
    auto plaintext = decrypt_authenticated(ciphertext, key, iv, algorithm, aad);
    // An AEAD authentication failure is an empty result, the documented contract of this overload.
    return plaintext ? std::move(*plaintext) : std::vector<unsigned char>{};
}

std::optional<std::vector<unsigned char>>
crypto::decrypt_authenticated(const std::vector<unsigned char> &ciphertext, const std::vector<unsigned char> &key,
                              const std::vector<unsigned char> &iv, SymmetricAlgorithm algorithm, const std::vector<unsigned char> &aad) {
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

    validate_symmetric_parameters(cipher, key, iv, is_aead);

    // Check if there's enough data for ciphertext + tag
    if (is_aead && ciphertext.size() < static_cast<size_t>(tag_len)) {
        throw std::runtime_error("Ciphertext too short for AEAD mode");
    }

    // Owned the line it is created: the allocations below may throw (Huly QB-345).
    const detail::openssl_ptr<EVP_CIPHER_CTX> ctx{EVP_CIPHER_CTX_new()};
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // The ciphertext proper is read in place -- no copy of a buffer that may be gigabytes -- and the
    // AEAD tag that ends it (16 bytes for every AEAD above, 0 otherwise) is copied out:
    // EVP_CTRL_GCM_SET_TAG takes a writable pointer.
    const std::size_t             data_len = ciphertext.size() - static_cast<std::size_t>(tag_len);
    std::array<unsigned char, 16> tag{};
    std::copy_n(ciphertext.data() + data_len, tag_len, tag.data());

    std::vector<unsigned char> plaintext(data_len + EVP_MAX_BLOCK_LENGTH);
    std::size_t                total = 0;

    if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, key.data(), iv.data()) != 1) {
        throw std::runtime_error("Failed to initialize decryption: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // The AAD, then the ciphertext, through EVP_DecryptUpdate's int length a chunk at a time (Huly QB-973).
    if (is_aead && !aad.empty() && !detail::for_each_openssl_chunk(aad.size(), [&](std::size_t offset, int length) {
            int ignored = 0;
            return EVP_DecryptUpdate(ctx.get(), nullptr, &ignored, aad.data() + offset, length) == 1;
        })) {
        throw std::runtime_error("Failed to process AAD: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());       // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (is_aead && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag_len, tag.data()) != 1) {
        throw std::runtime_error("Failed to set authentication tag: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                  // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (!detail::for_each_openssl_chunk(data_len, [&](std::size_t offset, int length) {
            int written = 0;
            if (EVP_DecryptUpdate(ctx.get(), plaintext.data() + total, &written, ciphertext.data() + offset, length) != 1)
                return false;
            total += static_cast<std::size_t>(written);
            return true;
        })) {
        throw std::runtime_error("Failed to decrypt data: " + get_openssl_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // For AEAD modes, finalisation is also where the tag is verified.
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + total, &final_len) != 1) {
        if (is_aead)
            return std::nullopt; // authentication failed
        throw std::runtime_error("Failed to finalize decryption: " + get_openssl_error());
    }
    total += static_cast<std::size_t>(final_len);

    plaintext.resize(total);
    return plaintext;
}

// Generic hash implementation
std::vector<unsigned char>
crypto::hash(const std::vector<unsigned char> &data, DigestAlgorithm algorithm) {
    const EVP_MD *md = get_evp_md(algorithm);
    if (!md) {
        throw std::runtime_error("Unknown digest algorithm");
    }

    const detail::openssl_ptr<EVP_MD_CTX> ctx{EVP_MD_CTX_new()};
    if (!ctx) {
        throw std::runtime_error("Failed to create digest context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
        throw std::runtime_error("Failed to initialize digest: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());             // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // EVP_DigestUpdate takes a size_t: no chunking needed.
    if (EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("Failed to update digest: " + get_openssl_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> digest(static_cast<std::size_t>(EVP_MD_size(md)));
    unsigned int               digest_len = 0;

    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
        throw std::runtime_error("Failed to finalize digest: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    digest.resize(digest_len);
    return digest;
}

// HMAC implementation
std::vector<unsigned char>
crypto::hmac(const std::vector<unsigned char> &data, const std::vector<unsigned char> &key, DigestAlgorithm algorithm) {
    const EVP_MD *md = get_evp_md(algorithm);
    if (!md) {
        throw std::runtime_error("Unknown digest algorithm");
    }

    const detail::openssl_ptr<EVP_MD_CTX> mdctx{EVP_MD_CTX_new()};
    if (!mdctx) {
        throw std::runtime_error("Failed to create digest context: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());                 // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Both handles are owned, so no path frees anything by hand. That is load-bearing here: when
    // `mdctx` was freed by an enclosing `catch` AND by every error path inside the `try`, the most
    // ordinary misconfiguration there is -- an EMPTY key, `EVP_PKEY_new_mac_key(..., nullptr, 0)`
    // failing -- was a deterministic DOUBLE FREE: a service whose JWT secret env var was unset
    // corrupted its heap on the first `jwt::create()` / `jwt::verify()`.
    const detail::openssl_ptr<EVP_PKEY> pkey{EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, nullptr, key.data(),
                                                                  detail::openssl_int_length(key.size(), "HMAC key"))};
    if (!pkey) {
        throw std::runtime_error("Failed to create HMAC key: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    if (EVP_DigestSignInit(mdctx.get(), nullptr, md, nullptr, pkey.get()) != 1) {
        throw std::runtime_error("Failed to initialize HMAC: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());           // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // EVP_DigestSignUpdate takes a size_t: no chunking needed.
    if (EVP_DigestSignUpdate(mdctx.get(), data.data(), data.size()) != 1) {
        throw std::runtime_error("Failed to update HMAC: " + get_openssl_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    size_t hmac_len = 0;
    if (EVP_DigestSignFinal(mdctx.get(), nullptr, &hmac_len) != 1) {
        throw std::runtime_error("Failed to determine HMAC size: " + // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                                 get_openssl_error());               // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    std::vector<unsigned char> hmac_value(hmac_len);
    if (EVP_DigestSignFinal(mdctx.get(), hmac_value.data(), &hmac_len) != 1) {
        throw std::runtime_error("Failed to finalize HMAC: " + get_openssl_error()); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    hmac_value.resize(hmac_len);
    return hmac_value;
}

} // namespace qb
