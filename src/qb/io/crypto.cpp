/**
 * @file qb/io/crypto.cpp
 * @brief Implementation of cryptographic utilities
 *
 * This file contains the implementation of various cryptographic functions
 * including hash functions (MD5, SHA1, SHA256, SHA512), encoding/decoding
 * (Base64, Hex), and key derivation (PBKDF2). It provides a comprehensive
 * set of cryptographic utilities for the QB framework.
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

#include <qb/io/crypto.h>

namespace qb {

// Base64-decodes one chunk of text -- whole 4-character quanta, at most detail::openssl_max_chunk
// characters -- into `out`, the way both decoders always decoded: one BIO_read through a base64 BIO over a
// memory BIO. BIO_new_mem_buf and BIO_read take an int length, so text past INT_MAX is decoded chunk by
// chunk (Huly QB-973); base64 decodes quantum by quantum, so the chunks' outputs concatenate to the decode
// of the whole, and text up to INT_MAX is one chunk, exactly the former single call.
// Returns the bytes written (0 when nothing decodes) or -1 on an OpenSSL failure.
static int
base64_decode_chunk(const char *text, int length, unsigned char *out, std::size_t capacity) noexcept {
    const detail::openssl_ptr<BIO> chain{BIO_new(BIO_f_base64())};
    BIO *const                     source = BIO_new_mem_buf(text, length);
    if (!chain || !source) {
        BIO_free(source); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        return -1;        // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    BIO_set_flags(chain.get(), BIO_FLAGS_BASE64_NO_NL);
    BIO_push(chain.get(), source); // the chain, `source` with it, is owned by its head from here
    return BIO_read(chain.get(), out, static_cast<int>(capacity < detail::openssl_max_chunk ? capacity : detail::openssl_max_chunk));
}

// Base64-encodes `len` bytes into `out`: exactly (len + 2) / 3 * 4 characters, the standard alphabet, '='
// padding, no newline -- the text the base64 BIO wrote. EVP_EncodeBlock takes an int and its output must
// fit one, so the input goes a chunk of whole 3-byte groups at a time: each chunk encodes to exactly its
// share of the text and only the last one carries padding (Huly QB-973). Writing straight into the
// caller's buffer also avoids the memory BIO, which refuses to grow past ~1.6 GB (BUF_MEM's
// LIMIT_BEFORE_EXPANSION): through it, no input past ~1.2 GB could be encoded at all.
static void
base64_encode_into(char *out, const unsigned char *in, std::size_t len) noexcept {
    constexpr std::size_t whole_groups = std::size_t{3} << 28; // 768 MiB of input, 1 GiB of text per call
    (void) detail::for_each_openssl_chunk(
        len,
        [&](std::size_t offset, int length) {
            // EVP_EncodeBlock NUL-terminates: the next chunk overwrites the NUL, and after the last one it
            // lands on the string's own terminator.
            out += EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out), in + offset, length);
            return true;
        },
        whole_groups);
}

std::string
crypto::base64::encode(const std::string &input) noexcept {
    std::string base64((input.size() + 2) / 3 * 4, '\0');
    base64_encode_into(base64.data(), reinterpret_cast<const unsigned char *>(input.data()), input.size());
    return base64;
}

/// Returns Base64 decoded string from base64 input.
std::string
crypto::base64::decode(const std::string &base64) noexcept {
    std::string ascii;

    // Resize ascii, however, the size is a up to two bytes too large.
    ascii.resize((6 * base64.size()) / 8);

    std::size_t total   = 0;
    const bool  decoded = detail::for_each_openssl_chunk(base64.size(), [&](std::size_t offset, int length) {
        const int read =
            base64_decode_chunk(base64.data() + offset, length, reinterpret_cast<unsigned char *>(ascii.data()) + total, ascii.size() - total);
        if (read < 0)
            return false;
        total += static_cast<std::size_t>(read);
        return true;
    });
    if (decoded && total > 0)
        ascii.resize(total);
    else
        ascii.clear();

    return ascii;
}

std::string
crypto::evp(std::istream &stream, const EVP_MD *md) noexcept {
    const detail::openssl_ptr<EVP_MD_CTX> context{EVP_MD_CTX_new()};
    std::string                           hash;

    if (context && EVP_DigestInit_ex(context.get(), md, NULL)) {
        std::streamsize   read_length;
        std::vector<char> buffer(buffer_size);
        while ((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
            EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(read_length));
        unsigned int hash_len = 0;
        hash.resize(EVP_MAX_MD_SIZE);
        // Check the return value: on failure hash_len would be left
        // uninitialized and hash.resize(hash_len) could request a garbage size.
        // Treat failure as an empty hash (fail-closed, matching the empty
        // result returned when the context/init fails above).
        if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char *>(hash.data()), &hash_len) == 1)
            hash.resize(hash_len);
        else
            hash.clear();
    }
    return hash;
}

DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED
/// Returns md5 hash value from input string.
std::string
crypto::md5(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(MD5_DIGEST_LENGTH);
    MD5(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns md5 hash value from input stream.
std::string
crypto::md5(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("MD5"));

    for (std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}
DISABLE_WARNING_POP

/// Returns sha1 hash value from input string.
std::string
crypto::sha1(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA_DIGEST_LENGTH);
    SHA1(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha1 hash value from input stream.
std::string
crypto::sha1(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA1"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha256 hash value from input string.
std::string
crypto::sha256(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha256 hash value from input stream.
std::string
crypto::sha256(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA256"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha512 hash value from input string.
std::string
crypto::sha512(const std::string &input, std::size_t iterations) noexcept {
    std::string hash;

    hash.resize(SHA512_DIGEST_LENGTH);
    SHA512(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

    return hash;
}

/// Returns sha512 hash value from input stream.
std::string
crypto::sha512(std::istream &stream, std::size_t iterations) noexcept {
    std::string hash = evp(stream, EVP_get_digestbyname("SHA512"));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

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
crypto::pbkdf2(const std::string &password, const std::string &salt, int iterations, int key_size) noexcept {
    std::string key;
    if (key_size <= 0)
        return key;
    // The password and salt lengths are OpenSSL ints: one past INT_MAX would wrap (Huly QB-973), and
    // this noexcept API reports a failure as an empty key.
    constexpr auto int_max = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (password.size() > int_max || salt.size() > int_max)
        return key;
    key.resize(static_cast<std::size_t>(key_size));
    // Check the return value: on failure the buffer would be returned
    // uninitialized and silently used as key material.
    if (PKCS5_PBKDF2_HMAC_SHA1(password.c_str(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.c_str()),
                               static_cast<int>(salt.size()), iterations, key_size, reinterpret_cast<unsigned char *>(&key[0]))
        != 1) {
        key.clear(); // empty result signals failure (noexcept contract)
    }
    return key;
}

// base64 encode (without new line)
std::string
crypto::base64_encode(const unsigned char *data, size_t len) {
    std::string encoded((len + 2) / 3 * 4, '\0');
    base64_encode_into(encoded.data(), data, len); // see base64_encode_into(): no BIO, no int length wrapped
    return encoded;
}
// base64 decode
std::vector<unsigned char>
crypto::base64_decode(const std::string &input) {
    // Empty input decodes to empty output. BIO_read on a zero-length mem buf
    // can report 0/-1, which the failure check below would otherwise treat as
    // an error — short-circuit so the empty round-trip is symmetric with encode.
    if (input.empty())
        return std::vector<unsigned char>{};
    std::vector<unsigned char> decoded(input.size());
    std::size_t                total = 0;
    // Chunk by chunk past INT_MAX (Huly QB-973): see base64_decode_chunk().
    if (!detail::for_each_openssl_chunk(input.size(), [&](std::size_t offset, int length) {
            const int read = base64_decode_chunk(input.data() + offset, length, decoded.data() + total, decoded.size() - total);
            if (read < 0)
                return false;
            total += static_cast<std::size_t>(read);
            return true;
        })) {
        throw std::runtime_error("Error reading BIO");
    }
    decoded.resize(total);
    return decoded;
}
// HMAC-SHA256 en using modern openssl api
std::vector<unsigned char>
crypto::hmac_sha256(const std::vector<unsigned char> &key, const std::string &data) {
    // Récupération de l'algorithme "HMAC"
    const detail::openssl_ptr<EVP_MAC> mac{EVP_MAC_fetch(nullptr, "HMAC", nullptr)};
    if (!mac) {
        throw std::runtime_error("EVP_MAC_fetch failed");
    }
    const detail::openssl_ptr<EVP_MAC_CTX> ctx{EVP_MAC_CTX_new(mac.get())};
    if (!ctx) {
        throw std::runtime_error("EVP_MAC_CTX_new failed"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    // Spécifier l'algorithme de hachage à utiliser : "SHA256"
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string("digest", const_cast<char *>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_end();
    if (EVP_MAC_init(ctx.get(), key.data(), key.size(), params) != 1) {
        throw std::runtime_error("EVP_MAC_init failed"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    if (EVP_MAC_update(ctx.get(), reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 1) {
        throw std::runtime_error("EVP_MAC_update failed"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    size_t                     out_len = 0;
    std::vector<unsigned char> result(EVP_MAX_MD_SIZE); // taille maximale possible
    if (EVP_MAC_final(ctx.get(), result.data(), &out_len, result.size()) != 1) {
        throw std::runtime_error("EVP_MAC_final failed"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }
    result.resize(out_len);
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

// Generate cryptographically secure random string using OpenSSL RAND_bytes
std::string
crypto::generate_secure_random_string(std::size_t len, std::string_view range) {
    if (range.empty()) {
        throw std::invalid_argument("Character range cannot be empty");
    }
    // A single random byte (0..255) cannot uniformly index a set larger than
    // 256. With range.size() > 256 the rejection threshold (256/range_size)*
    // range_size below evaluates to 0, so the do/while rejection loop would
    // spin forever (every byte is >= 0). Reject oversized ranges explicitly.
    if (range.size() > 256) {
        throw std::invalid_argument("Character range must not exceed 256 characters");
    }
    if (len == 0) {
        return "";
    }

    std::string       result(len, '\0');
    const std::size_t range_size = range.size();

    // Generate random bytes using OpenSSL's CSPRNG
    // We generate more bytes than needed to handle bias from modulo operation
    const std::size_t          bytes_needed = len * 2; // Extra for bias correction
    std::vector<unsigned char> random_bytes(bytes_needed);

    // secure_random_fill() hands RAND_bytes' int length a chunk at a time (Huly QB-344).
    if (!secure_random_fill(random_bytes)) {
        throw std::runtime_error("Failed to generate secure random bytes"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
    }

    // Map random bytes to character range using rejection sampling for uniform distribution
    std::size_t random_idx = 0;
    for (std::size_t i = 0; i < len; ++i) {
        // Use rejection sampling to avoid modulo bias
        // Keep trying until we get a value in the valid range
        unsigned char random_val;
        do {
            if (random_idx >= bytes_needed) {
                // Need more random bytes
                if (!secure_random_fill(random_bytes)) {
                    throw std::runtime_error("Failed to generate additional random bytes"); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
                }
                random_idx = 0;
            }
            random_val = random_bytes[random_idx++];
        } while (random_val >= (256 / range_size) * range_size);

        result[i] = range[random_val % range_size];
    }

    return result;
}

} // namespace qb
