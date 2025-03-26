/**
 * @file qb/io/crypto.h
 * @brief Cryptographic utilities for the QB IO library
 * 
 * This file provides cryptographic functionality using the OpenSSL library.
 * It includes support for common cryptographic operations such as hashing
 * (MD5, SHA1, SHA256, SHA512), base64 encoding/decoding, random string 
 * generation, and key derivation (PBKDF2).
 * 
 * The implementation is encapsulated in the crypto class, which provides
 * both static methods for direct use and nested classes for specific 
 * cryptographic operations.
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
/**
 * @brief Custom implementation of round() function for MSVS 2012
 * @param x The value to round
 * @return The rounded value
 */
inline double
round(double x) noexcept { // Custom definition of round() for positive numbers
    return floor(x + 0.5);
}
#endif

/**
 * @class crypto
 * @brief Provides cryptographic operations and utilities
 * 
 * This class serves as a container for cryptographic operations including
 * hashing, encoding, encryption, and random string generation. It provides
 * a collection of static methods that can be called directly.
 */
class crypto {
    /** @brief Size of the buffer used for cryptographic operations */
    const static std::size_t buffer_size = 131072;

public:
    /** @brief Character range for numeric values (0-9) */
    constexpr static const std::string_view range_numeric = "0123456789";
    
    /** @brief Character range for alphabetic values (A-Z, a-z) */
    constexpr static const std::string_view range_alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                          "abcdefghijklmnopqrstuvwxyz";
    
    /** @brief Character range for lowercase alphabetic values (a-z) */
    constexpr static const std::string_view range_alpha_lower = "abcdefghijklmnopqrstuvwxyz";
    
    /** @brief Character range for uppercase alphabetic values (A-Z) */
    constexpr static const std::string_view range_alpha_upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    
    /** @brief Character range for alphanumeric values (0-9, A-Z, a-z) */
    constexpr static const std::string_view range_alpha_numeric =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    /** @brief Character range for alphanumeric values and special characters */
    constexpr static const std::string_view range_alpha_numeric_special =
        "0123456789"
        " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    /** @brief Character range for uppercase hexadecimal values (0-9, A-F) */
    constexpr static const std::string_view range_hex_upper = "0123456789ABCDEF";
    
    /** @brief Character range for lowercase hexadecimal values (0-9, a-f) */
    constexpr static const std::string_view range_hex_lower = "0123456789abcdef";

    /**
     * @brief Create a cryptographically secure random number generator
     * 
     * Creates a random number generator of type T (defaulting to mt19937)
     * seeded with a secure random seed sequence.
     * 
     * @tparam T The type of random number generator to create (default: std::mt19937)
     * @return A securely seeded random number generator
     */
    template <typename T = std::mt19937>
    static auto
    random_generator() {
        auto constexpr seed_bytes = sizeof(typename T::result_type) * T::state_size;
        auto constexpr seed_len = seed_bytes / sizeof(std::seed_seq::result_type);
        auto seed = std::array<std::seed_seq::result_type, seed_len>();
        auto dev = std::random_device();
        std::generate_n(std::begin(seed), seed_len, std::ref(dev));
        auto seed_seq = std::seed_seq(std::begin(seed), std::end(seed));
        return T{seed_seq};
    }

    /**
     * @brief Generate a random string using characters from the specified range
     * 
     * Creates a random string of the specified length using only characters
     * from the provided range.
     * 
     * @tparam T The type of the character range
     * @param len The length of the random string to generate
     * @param range The range of characters to use in the string
     * @return A random string of the specified length
     */
    template <typename T>
    static std::string
    generate_random_string(std::size_t len, T const &range) {
        thread_local auto rng = random_generator<>();
        auto dist = std::uniform_int_distribution{{}, range.size() - 1};
        auto result = std::string(len, '\0');
        std::generate_n(std::begin(result), len, [&]() { return range[dist(rng)]; });
        return result;
    }

    /**
     * @brief Generate a random string using characters from the specified array
     * 
     * Creates a random string of the specified length using only characters
     * from the provided array.
     * 
     * @tparam T The type of the character array elements
     * @tparam N The size of the character array
     * @param len The length of the random string to generate
     * @param range The array of characters to use in the string
     * @return A random string of the specified length
     */
    template <typename T, std::size_t N>
    std::string
    generate_random_string(std::size_t len, const T range[N]) {
        return generate_random_string(len, std::string_view(range, sizeof(range) - 1));
    }

    /**
     * @class base64
     * @brief Base64 encoding and decoding utilities
     * 
     * This nested class provides methods for encoding and decoding data
     * in Base64 format, which is commonly used for representing binary data
     * in a text format that can be safely transmitted in environments that only
     * support ASCII.
     */
    class base64 {
    public:
        /**
         * @brief Encode a string to Base64
         * 
         * Converts a string containing arbitrary binary data to a Base64-encoded
         * string that contains only ASCII characters.
         * 
         * @param input The string to encode
         * @return The Base64-encoded string
         */
        static std::string encode(const std::string &input) noexcept;

        /**
         * @brief Decode a Base64 string
         * 
         * Converts a Base64-encoded string back to its original binary form.
         * 
         * @param base64 The Base64-encoded string to decode
         * @return The decoded binary string
         */
        static std::string decode(const std::string &base64) noexcept;
    };

    /**
     * @brief Convert a binary string to a hexadecimal string
     * 
     * Converts each byte in the input string to a two-character hexadecimal
     * representation, using characters from the specified range.
     * 
     * @param input The binary string to convert
     * @param range The range of characters to use for hexadecimal digits (default: uppercase)
     * @return The hexadecimal string representation
     */
    static std::string to_hex_string(const std::string &input, std::string_view const &range = range_hex_upper) noexcept;
    
    /**
     * @brief Get the numeric value of a hexadecimal digit
     * 
     * Converts a hexadecimal digit character to its numeric value (0-15).
     * 
     * @param hex_digit The hexadecimal digit character
     * @return The numeric value of the digit, or -1 if the character is not a valid hex digit
     */
    static int hex_value(unsigned char hex_digit) noexcept;
    
    /**
     * @brief Convert a hexadecimal string to a formatted string
     * 
     * Converts a string of hexadecimal bytes to a formatted string representation.
     * 
     * @param input The hexadecimal string to convert
     * @return The formatted string
     */
    static std::string hex_to_string(const std::string &input) noexcept;
    
    /**
     * @brief Calculate a hash from an input stream using a specified digest algorithm
     * 
     * Computes a cryptographic hash of the data in the input stream using
     * the specified OpenSSL message digest.
     * 
     * @param stream The input stream containing the data to hash
     * @param md The OpenSSL message digest to use
     * @return The calculated hash as a hexadecimal string
     */
    static std::string evp(std::istream &stream, const EVP_MD *md) noexcept;
    
    /**
     * @brief Calculate an MD5 hash of a string
     * 
     * Computes the MD5 hash of the input string, optionally applying
     * the hash function multiple times for extra security.
     * 
     * @param input The string to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The MD5 hash as a hexadecimal string
     */
    static std::string md5(const std::string &input,
                           std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate an MD5 hash of data from an input stream
     * 
     * Computes the MD5 hash of the data in the input stream, optionally
     * applying the hash function multiple times for extra security.
     * 
     * @param stream The input stream containing the data to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The MD5 hash as a hexadecimal string
     */
    static std::string md5(std::istream &stream, std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-1 hash of a string
     * 
     * Computes the SHA-1 hash of the input string, optionally applying
     * the hash function multiple times for extra security.
     * 
     * @param input The string to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-1 hash as a hexadecimal string
     */
    static std::string sha1(const std::string &input,
                            std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-1 hash of data from an input stream
     * 
     * Computes the SHA-1 hash of the data in the input stream, optionally
     * applying the hash function multiple times for extra security.
     * 
     * @param stream The input stream containing the data to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-1 hash as a hexadecimal string
     */
    static std::string sha1(std::istream &stream, std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-256 hash of a string
     * 
     * Computes the SHA-256 hash of the input string, optionally applying
     * the hash function multiple times for extra security.
     * 
     * @param input The string to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-256 hash as a hexadecimal string
     */
    static std::string sha256(const std::string &input,
                              std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-256 hash of data from an input stream
     * 
     * Computes the SHA-256 hash of the data in the input stream, optionally
     * applying the hash function multiple times for extra security.
     * 
     * @param stream The input stream containing the data to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-256 hash as a hexadecimal string
     */
    static std::string sha256(std::istream &stream, std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-512 hash of a string
     * 
     * Computes the SHA-512 hash of the input string, optionally applying
     * the hash function multiple times for extra security.
     * 
     * @param input The string to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-512 hash as a hexadecimal string
     */
    static std::string sha512(const std::string &input,
                              std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Calculate a SHA-512 hash of data from an input stream
     * 
     * Computes the SHA-512 hash of the data in the input stream, optionally
     * applying the hash function multiple times for extra security.
     * 
     * @param stream The input stream containing the data to hash
     * @param iterations Number of times to apply the hash function (default: 1)
     * @return The SHA-512 hash as a hexadecimal string
     */
    static std::string sha512(std::istream &stream, std::size_t iterations = 1) noexcept;
    
    /**
     * @brief Derive a key using PBKDF2
     * 
     * Uses the PBKDF2 (Password-Based Key Derivation Function 2) algorithm
     * to derive a key from a password and salt.
     * 
     * @param password The password to derive the key from
     * @param salt The salt to use in the derivation process
     * @param iterations Number of iterations to use in the derivation
     * @param key_size Size of the derived key in bytes
     * @return The derived key as a string
     */
    static std::string pbkdf2(const std::string &password, const std::string &salt,
                              int iterations, int key_size) noexcept;
    
    /**
     * @brief Encode binary data to Base64
     * 
     * Converts binary data to a Base64-encoded string. This method does not
     * include newline characters in the output.
     * 
     * @param data Pointer to the binary data to encode
     * @param len Length of the binary data in bytes
     * @return The Base64-encoded string
     */
    static std::string base64_encode(const unsigned char* data, size_t len);
    
    /**
     * @brief Decode a Base64 string to binary data
     * 
     * Converts a Base64-encoded string back to its original binary form.
     * 
     * @param input The Base64-encoded string to decode
     * @return The decoded binary data as a vector of unsigned chars
     */
    static std::vector<unsigned char> base64_decode(const std::string& input);
    
    /**
     * @brief Calculate an HMAC-SHA256 hash
     * 
     * Computes an HMAC-SHA256 hash of the input data using the provided key.
     * 
     * @param key The key to use for the HMAC computation
     * @param data The data to hash
     * @return The HMAC-SHA256 hash as a vector of unsigned chars
     */
    static std::vector<unsigned char> hmac_sha256(const std::vector<unsigned char>& key, const std::string& data);
    
    /**
     * @brief Calculate a SHA-256 hash of binary data
     * 
     * Computes the SHA-256 hash of the input binary data.
     * 
     * @param data The binary data to hash
     * @return The SHA-256 hash as a vector of unsigned chars
     */
    static std::vector<unsigned char> sha256(const std::vector<unsigned char>& data);
    
    /**
     * @brief XOR two byte arrays
     * 
     * Performs a bitwise XOR operation between corresponding bytes of two arrays.
     * 
     * @param a The first byte array
     * @param b The second byte array
     * @return The result of XORing the two arrays
     * @throws std::runtime_error if the arrays have different sizes
     */
    static std::vector<unsigned char> xor_bytes(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b);
};
} // namespace qb

#endif // QB_IO_CRYPTO_H
