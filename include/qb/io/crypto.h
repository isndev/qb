/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_IO_CRYPTO_H
#define QB_IO_CRYPTO_H

#ifndef QB_IO_WITH_SSL
#    error "missing OpenSSL Library"
#endif

#include <cmath>
#include <iomanip>
#include <istream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

#undef hex_to_string

namespace qb {

#if _MSC_VER == 1700 // MSVS 2012 has no definition for round()
inline double
round(double x) noexcept { // Custom definition of round() for positive numbers
    return floor(x + 0.5);
}
#endif

class crypto {
    const static std::size_t buffer_size = 131072;

public:
    constexpr static const std::string_view range_numeric = "0123456789";
    constexpr static const std::string_view range_alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                          "abcdefghijklmnopqrstuvwxyz";
    constexpr static const std::string_view range_alpha_lower = "abcdefghijklmnopqrstuvwxyz";
    constexpr static const std::string_view range_alpha_upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr static const std::string_view range_alpha_numeric =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    constexpr static const std::string_view range_alpha_numeric_special =
        "0123456789"
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    constexpr static const std::string_view range_hex_upper = "0123456789ABCDEF";
    constexpr static const std::string_view range_hex_lower = "0123456789abcdef";

    template <typename T = std::mt19937>
    static auto
    random_generator() {
        auto constexpr seed_bytes = sizeof(typename T::result_type) * T::state_size;
        auto constexpr seed_len = seed_bytes / sizeof(std::seed_seq::result_type);
        auto seed = std::array<std::seed_seq::result_type, seed_len>();
        auto dev = std::random_device();
        std::generate_n(begin(seed), seed_len, std::ref(dev));
        auto seed_seq = std::seed_seq(begin(seed), end(seed));
        return T{seed_seq};
    }

    template <typename T>
    static std::string
    generate_random_string(std::size_t len, T const &range) {
        thread_local auto rng = random_generator<>();
        auto dist = std::uniform_int_distribution{{}, range.size() - 1};
        auto result = std::string(len, '\0');
        std::generate_n(std::begin(result), len, [&]() { return range[dist(rng)]; });
        return result;
    }

    template <typename T, std::size_t N>
    std::string
    generate_random_string(std::size_t len, const T range[N]) {
        return generate_random_string(len, std::string_view(range, sizeof(range) - 1));
    }

    class base64 {
    public:
        /// Returns Base64 encoded string from input string.
        static std::string encode(const std::string &input) noexcept;

        /// Returns Base64 decoded string from base64 input.
        static std::string decode(const std::string &base64) noexcept;
    };

    /// Returns hex string from bytes in input string.
    static std::string to_hex_string(const std::string &input, std::string_view const &range = range_hex_upper) noexcept;
    /// Returns hex value from byte.
    static int hex_value(unsigned char hex_digit) noexcept;
    /// Returns formatted hex string from hex bytes in input string.
    static std::string hex_to_string(const std::string &input) noexcept;
    /// Returns hash from input stream using EVP_get_digest_byname
    static std::string evp(std::istream &stream, const EVP_MD *md) noexcept;
    /// Returns md5 hash value from input string.
    static std::string md5(const std::string &input,
                           std::size_t iterations = 1) noexcept;
    /// Returns md5 hash value from input stream.
    static std::string md5(std::istream &stream, std::size_t iterations = 1) noexcept;
    /// Returns sha1 hash value from input string.
    static std::string sha1(const std::string &input,
                            std::size_t iterations = 1) noexcept;
    /// Returns sha1 hash value from input stream.
    static std::string sha1(std::istream &stream, std::size_t iterations = 1) noexcept;
    /// Returns sha256 hash value from input string.
    static std::string sha256(const std::string &input,
                              std::size_t iterations = 1) noexcept;
    /// Returns sha256 hash value from input stream.
    static std::string sha256(std::istream &stream, std::size_t iterations = 1) noexcept;
    /// Returns sha512 hash value from input string.
    static std::string sha512(const std::string &input,
                              std::size_t iterations = 1) noexcept;
    /// Returns sha512 hash value from input stream.
    static std::string sha512(std::istream &stream, std::size_t iterations = 1) noexcept;
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
    static std::string pbkdf2(const std::string &password, const std::string &salt,
                              int iterations, int key_size) noexcept;
    // base64 encode (without new line)
    static std::string base64_encode(const unsigned char* data, size_t len);
    // base64 decode
    static std::vector<unsigned char> base64_decode(const std::string& input);
    // HMAC-SHA256 en using modern openssl api
    static std::vector<unsigned char> hmac_sha256(const std::vector<unsigned char>& key, const std::string& data);
    // SHA256 with std::vector
    static std::vector<unsigned char> sha256(const std::vector<unsigned char>& data);
    // xor two vector of same size
    static std::vector<unsigned char> xor_bytes(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b);
};
} // namespace qb

#endif // QB_IO_CRYPTO_H
