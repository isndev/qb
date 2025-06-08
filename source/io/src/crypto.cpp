/**
 * @file qb/io/src/crypto.cpp
 * @brief Implementation of cryptographic utilities
 *
 * This file contains the implementation of various cryptographic functions
 * including hash functions (MD5, SHA1, SHA256, SHA512), encoding/decoding
 * (Base64, Hex), and key derivation (PBKDF2). It provides a comprehensive
 * set of cryptographic utilities for the QB framework.
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

#include <qb/io/crypto.h>

namespace qb {

std::string
crypto::base64::encode(const std::string &input) noexcept {
    std::string base64;

    BIO     *bio, *b64;
    BUF_MEM *bptr = BUF_MEM_new();

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_new(BIO_s_mem());
    BIO_push(b64, bio);
    BIO_set_mem_buf(b64, bptr, BIO_CLOSE);

    // Write directly to base64-buffer to avoid copy
    auto base64_length = static_cast<std::size_t>(
        round(4 * ceil(static_cast<double>(input.size()) / 3.0)));
    base64.resize(base64_length);
    bptr->length = 0;
    bptr->max    = base64_length + 1;
    bptr->data   = &base64[0];

    if (BIO_write(b64, &input[0], static_cast<int>(input.size())) <= 0 ||
        BIO_flush(b64) <= 0)
        base64.clear();

    // To keep &base64[0] through BIO_free_all(b64)
    bptr->length = 0;
    bptr->max    = 0;
    bptr->data   = nullptr;

    BIO_free_all(b64);

    return base64;
}

/// Returns Base64 decoded string from base64 input.
std::string
crypto::base64::decode(const std::string &base64) noexcept {
    std::string ascii;

    // Resize ascii, however, the size is a up to two bytes too large.
    ascii.resize((6 * base64.size()) / 8);
    BIO *b64, *bio;

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
// TODO: Remove in 2022 or later
#if (defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x1000214fL) || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2080000fL)
    bio =
        BIO_new_mem_buf(const_cast<char *>(&base64[0]), static_cast<int>(base64.size()));
#else
    bio = BIO_new_mem_buf(&base64[0], static_cast<int>(base64.size()));
#endif
    bio = BIO_push(b64, bio);

    auto decoded_length = BIO_read(bio, &ascii[0], static_cast<int>(ascii.size()));
    if (decoded_length > 0)
        ascii.resize(static_cast<std::size_t>(decoded_length));
    else
        ascii.clear();

    BIO_free_all(b64);

    return ascii;
}

/// Returns hex string from bytes in input string.
std::string
crypto::to_hex_string(const std::string      &input,
                      std::string_view const &hex_digits) noexcept {
    std::string output;
    output.reserve(input.length() * 2);
    for (unsigned char c : input) {
        output.push_back(hex_digits[c >> 4]);
        output.push_back(hex_digits[c & 15]);
    }
    return output;
}

DISABLE_WARNING_PUSH
DISABLE_WARNING_NARROWING
/// Returns hex value from byte.
int
crypto::hex_value(unsigned char hex_digit) noexcept {
    static const char hex_values[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1,
        -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, 12,
        13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };

    return hex_values[hex_digit];
}
DISABLE_WARNING_POP

/// Returns formatted hex string from hex bytes in input string.
std::string
crypto::hex_to_string(const std::string &input) noexcept {
    const auto len = input.length();
    if (len & 1)
        return "";

    std::string output;
    output.reserve(len / 2);
    for (auto it = input.begin(); it != input.end();) {
        int hi = hex_value(*it++);
        int lo = hex_value(*it++);
        output.push_back(static_cast<char>(hi << 4 | lo));
    }
    return output;
}

std::string
crypto::evp(std::istream &stream, const EVP_MD *md) noexcept {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    std::string hash;

    if (context && EVP_DigestInit_ex(context, md, NULL)) {
        std::streamsize   read_length;
        std::vector<char> buffer(buffer_size);
        while ((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
            EVP_DigestUpdate(context, buffer.data(),
                             static_cast<std::size_t>(read_length));
        unsigned int hash_len;
        hash.resize(EVP_MAX_MD_SIZE);
        EVP_DigestFinal_ex(context, reinterpret_cast<unsigned char *>(hash.data()),
                           &hash_len);
        hash.resize(hash_len);
    }
    EVP_MD_CTX_free(context);
    return hash;
}

DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED
/// Returns md5 hash value from input string.
std::string
crypto::md5(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(MD5_DIGEST_LENGTH);
    MD5(reinterpret_cast<const unsigned char *>(&input[0]), input.size(),
        reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
            reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns md5 hash value from input stream.
std::string
crypto::md5(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("MD5"));

    for (std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
            reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}
DISABLE_WARNING_POP

/// Returns sha1 hash value from input string.
std::string
crypto::sha1(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA_DIGEST_LENGTH);
    SHA1(reinterpret_cast<const unsigned char *>(&input[0]), input.size(),
         reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
             reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha1 hash value from input stream.
std::string
crypto::sha1(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA1"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
             reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha256 hash value from input string.
std::string
crypto::sha256(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char *>(&input[0]), input.size(),
           reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
               reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha256 hash value from input stream.
std::string
crypto::sha256(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA256"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
               reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha512 hash value from input string.
std::string
crypto::sha512(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA512_DIGEST_LENGTH);
    SHA512(reinterpret_cast<const unsigned char *>(&input[0]), input.size(),
           reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
               reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha512 hash value from input stream.
std::string
crypto::sha512(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA512"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(),
               reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns PBKDF2 hash value from the given password
/// Input parameter key_size  number of bytes of the returned key.

/**
 * Returns PBKDF2 derived key from the given password.
 *
 * @param password   The password to derive key from.
 * @param salt       The salt to be used in the algorithm.
 * @param iterations Number of iterations to be used in the algorithm.
 * @param key_size   Number of bytes of the returned key.
 *
 * @return The PBKDF2 derived key.
 */
std::string
crypto::pbkdf2(const std::string &password, const std::string &salt, int iterations,
               int key_size) noexcept {
    std::string key;
    key.resize(static_cast<std::size_t>(key_size));
    PKCS5_PBKDF2_HMAC_SHA1(password.c_str(), static_cast<int>(password.size()),
                           reinterpret_cast<const unsigned char *>(salt.c_str()),
                           static_cast<int>(salt.size()), iterations, key_size,
                           reinterpret_cast<unsigned char *>(&key[0]));
    return key;
}

// base64 encode (without new line)
std::string
crypto::base64_encode(const unsigned char *data, size_t len) {
    BIO *bio = BIO_new(BIO_s_mem());
    BIO *b64 = BIO_new(BIO_f_base64());
    if (!bio || !b64) {
        throw std::runtime_error("Error during BIO creation");
    }
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL); // pas de retour à la ligne
    bio = BIO_push(b64, bio);
    if (BIO_write(bio, data, len) <= 0 || BIO_flush(bio) != 1) {
        BIO_free_all(bio);
        throw std::runtime_error("Error reading BIO");
    }
    BUF_MEM *bufferPtr = nullptr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    std::string encoded(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    return encoded;
}
// base64 decode
std::vector<unsigned char>
crypto::base64_decode(const std::string &input) {
    BIO *bio = BIO_new_mem_buf(input.data(), static_cast<int>(input.size()));
    BIO *b64 = BIO_new(BIO_f_base64());
    if (!bio || !b64) {
        throw std::runtime_error("Error during BIO creation");
    }
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    std::vector<unsigned char> decoded(input.size());
    int decodedLen = BIO_read(bio, decoded.data(), static_cast<int>(input.size()));
    if (decodedLen < 0) {
        BIO_free_all(bio);
        throw std::runtime_error("Error reading BIO");
    }
    decoded.resize(decodedLen);
    BIO_free_all(bio);
    return decoded;
}

// HMAC-SHA256 en using modern openssl api
std::vector<unsigned char>
crypto::hmac_sha256(const std::vector<unsigned char> &key, const std::string &data) {
    std::vector<unsigned char> result;
    // Récupération de l'algorithme "HMAC"
    EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac) {
        throw std::runtime_error("EVP_MAC_fetch failed");
    }
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    if (!ctx) {
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_CTX_new failed");
    }
    // Spécifier l'algorithme de hachage à utiliser : "SHA256"
    OSSL_PARAM params[2];
    params[0] =
        OSSL_PARAM_construct_utf8_string("digest", const_cast<char *>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_end();
    if (EVP_MAC_init(ctx, key.data(), key.size(), params) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_init failed");
    }
    if (EVP_MAC_update(ctx, reinterpret_cast<const unsigned char *>(data.data()),
                       data.size()) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_update failed");
    }
    size_t out_len     = 0;
    size_t out_buf_len = EVP_MAX_MD_SIZE; // taille maximale possible
    result.resize(out_buf_len);
    if (EVP_MAC_final(ctx, result.data(), &out_len, result.size()) != 1) {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
        throw std::runtime_error("EVP_MAC_final failed");
    }
    result.resize(out_len);
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    return result;
}
// SHA256 with std::vector
std::vector<unsigned char>
crypto::sha256(const std::vector<unsigned char> &data) {
    std::vector<unsigned char> digest(SHA256_DIGEST_LENGTH);
    if (!SHA256(data.data(), data.size(), digest.data())) {
        throw std::runtime_error("error during compute of SHA256");
    }
    return digest;
}

// xor two vector of same size
std::vector<unsigned char>
crypto::xor_bytes(const std::vector<unsigned char> &a,
                  const std::vector<unsigned char> &b) {
    if (a.size() != b.size()) {
        throw std::runtime_error("vectors must have the same size to XOR");
    }
    std::vector<unsigned char> result(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

} // namespace qb
