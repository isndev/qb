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
#include <stdexcept>
#include <optional>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/aes.h>
#include <openssl/rsa.h>

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

    /** @brief Character range for binary bytes (0-255) */
    constexpr static const std::string_view range_byte = "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
                                                         "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F"
                                                         "\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2A\x2B\x2C\x2D\x2E\x2F"
                                                         "\x30\x31\x32\x33\x34\x35\x36\x37\x38\x39\x3A\x3B\x3C\x3D\x3E\x3F"
                                                         "\x40\x41\x42\x43\x44\x45\x46\x47\x48\x49\x4A\x4B\x4C\x4D\x4E\x4F"
                                                         "\x50\x51\x52\x53\x54\x55\x56\x57\x58\x59\x5A\x5B\x5C\x5D\x5E\x5F"
                                                         "\x60\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6A\x6B\x6C\x6D\x6E\x6F"
                                                         "\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7A\x7B\x7C\x7D\x7E\x7F"
                                                         "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x8B\x8C\x8D\x8E\x8F"
                                                         "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9A\x9B\x9C\x9D\x9E\x9F"
                                                         "\xA0\xA1\xA2\xA3\xA4\xA5\xA6\xA7\xA8\xA9\xAA\xAB\xAC\xAD\xAE\xAF"
                                                         "\xB0\xB1\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xBB\xBC\xBD\xBE\xBF"
                                                         "\xC0\xC1\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xCB\xCC\xCD\xCE\xCF"
                                                         "\xD0\xD1\xD2\xD3\xD4\xD5\xD6\xD7\xD8\xD9\xDA\xDB\xDC\xDD\xDE\xDF"
                                                         "\xE0\xE1\xE2\xE3\xE4\xE5\xE6\xE7\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF"
                                                         "\xF0\xF1\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\xFA\xFB\xFC\xFD\xFE\xFF";

    /** @brief Supported symmetric cipher algorithms */
    enum class SymmetricAlgorithm {
        AES_128_CBC,
        AES_192_CBC,
        AES_256_CBC,
        AES_128_GCM,
        AES_192_GCM,
        AES_256_GCM,
        CHACHA20_POLY1305
    };

    /** @brief Supported digest algorithms */
    enum class DigestAlgorithm {
        MD5,
        SHA1,
        SHA224,
        SHA256,
        SHA384,
        SHA512,
        BLAKE2B512,
        BLAKE2S256
    };

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

    /**
     * @brief Generate cryptographically secure random bytes
     * 
     * Uses OpenSSL's RAND_bytes to generate secure random bytes.
     * 
     * @param size Number of random bytes to generate
     * @return Vector containing the random bytes
     */
    static std::vector<unsigned char> generate_random_bytes(size_t size);
    
    /**
     * @brief Generate a random initialization vector (IV)
     * 
     * Creates an IV of appropriate size for the specified algorithm.
     * 
     * @param algorithm The symmetric algorithm for which to generate the IV
     * @return Vector containing the random IV
     */
    static std::vector<unsigned char> generate_iv(SymmetricAlgorithm algorithm);
    
    /**
     * @brief Generate a random key
     * 
     * Creates a key of appropriate size for the specified algorithm.
     * 
     * @param algorithm The symmetric algorithm for which to generate the key
     * @return Vector containing the random key
     */
    static std::vector<unsigned char> generate_key(SymmetricAlgorithm algorithm);
    
    /**
     * @brief Encrypt data using a symmetric algorithm
     * 
     * Encrypts the provided data using the specified algorithm, key, and IV.
     * For GCM mode, also computes and appends an authentication tag.
     * 
     * @param plaintext The data to encrypt
     * @param key The encryption key
     * @param iv The initialization vector
     * @param algorithm The encryption algorithm to use
     * @param aad Additional authenticated data (for AEAD modes like GCM)
     * @return Vector containing the encrypted data
     */
    static std::vector<unsigned char> encrypt(
        const std::vector<unsigned char>& plaintext,
        const std::vector<unsigned char>& key,
        const std::vector<unsigned char>& iv,
        SymmetricAlgorithm algorithm,
        const std::vector<unsigned char>& aad = {});
    
    /**
     * @brief Decrypt data using a symmetric algorithm
     * 
     * Decrypts the provided data using the specified algorithm, key, and IV.
     * For GCM mode, also verifies the authentication tag.
     * 
     * @param ciphertext The data to decrypt
     * @param key The decryption key
     * @param iv The initialization vector
     * @param algorithm The encryption algorithm that was used
     * @param aad Additional authenticated data (for AEAD modes like GCM)
     * @return Vector containing the decrypted data or empty if authentication fails
     */
    static std::vector<unsigned char> decrypt(
        const std::vector<unsigned char>& ciphertext,
        const std::vector<unsigned char>& key,
        const std::vector<unsigned char>& iv,
        SymmetricAlgorithm algorithm,
        const std::vector<unsigned char>& aad = {});
    
    /**
     * @brief Compute a generic hash using the specified algorithm
     * 
     * @param data The data to hash
     * @param algorithm The hash algorithm to use
     * @return Vector containing the hash
     */
    static std::vector<unsigned char> hash(
        const std::vector<unsigned char>& data,
        DigestAlgorithm algorithm);
    
    /**
     * @brief Compute an HMAC using the specified algorithm
     * 
     * @param data The data to authenticate
     * @param key The authentication key
     * @param algorithm The hash algorithm to use
     * @return Vector containing the HMAC
     */
    static std::vector<unsigned char> hmac(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& key,
        DigestAlgorithm algorithm);
    
    /**
     * @brief Generate an RSA key pair
     * 
     * @param bits The key size in bits (e.g., 2048, 3072, 4096)
     * @return Pair of strings containing PEM-encoded private and public keys
     */
    static std::pair<std::string, std::string> generate_rsa_keypair(int bits = 2048);
    
    /**
     * @brief Generate an EC key pair
     * 
     * @param curve The curve name (e.g., "prime256v1", "secp384r1", "secp521r1")
     * @return Pair of strings containing PEM-encoded private and public keys
     */
    static std::pair<std::string, std::string> generate_ec_keypair(const std::string& curve = "prime256v1");
    
    /**
     * @brief Sign data using an RSA private key
     * 
     * @param data The data to sign
     * @param private_key PEM-encoded private key
     * @param digest The digest algorithm to use
     * @return Vector containing the signature
     */
    static std::vector<unsigned char> rsa_sign(
        const std::vector<unsigned char>& data,
        const std::string& private_key,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);
    
    /**
     * @brief Verify an RSA signature
     * 
     * @param data The data that was signed
     * @param signature The signature to verify
     * @param public_key PEM-encoded public key
     * @param digest The digest algorithm that was used
     * @return True if the signature is valid
     */
    static bool rsa_verify(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& signature,
        const std::string& public_key,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);
    
    /**
     * @brief Sign data using an EC private key
     * 
     * @param data The data to sign
     * @param private_key PEM-encoded private key
     * @param digest The digest algorithm to use
     * @return Vector containing the signature
     */
    static std::vector<unsigned char> ec_sign(
        const std::vector<unsigned char>& data,
        const std::string& private_key,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);
    
    /**
     * @brief Verify an EC signature
     * 
     * @param data The data that was signed
     * @param signature The signature to verify
     * @param public_key PEM-encoded public key
     * @param digest The digest algorithm that was used
     * @return True if the signature is valid
     */
    static bool ec_verify(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& signature,
        const std::string& public_key,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);
    
    /**
     * @brief Derive a shared secret using ECDH
     * 
     * @param private_key PEM-encoded EC private key
     * @param peer_public_key PEM-encoded EC public key
     * @return Vector containing the shared secret
     */
    static std::vector<unsigned char> ecdh_derive_secret(
        const std::string& private_key,
        const std::string& peer_public_key);
    
    /**
     * @brief Fill a vector with secure random bytes
     * 
     * @param buffer The vector to fill
     * @return True if successful
     */
    static bool secure_random_fill(std::vector<unsigned char>& buffer);
    
    /**
     * @brief Convert a digest algorithm enum to its corresponding EVP_MD
     * 
     * @param algorithm The digest algorithm
     * @return Pointer to the EVP_MD structure
     */
    static const EVP_MD* get_evp_md(DigestAlgorithm algorithm);

    // Nouveaux types pour les fonctions ajoutées
    /** @brief Paramètres pour l'algorithme Argon2 */
    struct Argon2Params {
        uint32_t t_cost;        // Coût en temps (nombre d'itérations)
        uint32_t m_cost;        // Coût en mémoire (KiB)
        uint32_t parallelism;   // Degré de parallélisme
        std::string salt;       // Sel (optionnel)
        
        Argon2Params() 
            : t_cost(3)
            , m_cost(1 << 16)
            , parallelism(1)
        {}
    };

    /** @brief Variantes de l'algorithme Argon2 */
    enum class Argon2Variant {
        Argon2d,    // Optimisé pour la résistance aux attaques GPU, mais vulnérable aux attaques par canal auxiliaire
        Argon2i,    // Optimisé pour la résistance aux attaques par canal auxiliaire
        Argon2id    // Hybride, recommandé pour la plupart des usages
    };

    /** @brief Algorithmes de dérivation de clé */
    enum class KdfAlgorithm {
        PBKDF2,
        HKDF,
        Argon2
    };

    /** @brief Modes d'opération pour le chiffrement par courbe elliptique */
    enum class ECIESMode {
        STANDARD,
        AES_GCM,
        CHACHA20
    };

    /** @brief Format pour les sorties du chiffrement par enveloppe */
    enum class EnvelopeFormat {
        RAW,        // Données brutes (IV + ciphertext + tag)
        JSON,       // Format JSON avec métadonnées
        BASE64      // Format base64 avec délimiteurs
    };

    /**
     * @brief Dérivation de clé basée sur Argon2
     *
     * Implémente l'algorithme Argon2 (2id par défaut) pour dériver une clé sécurisée à partir
     * d'un mot de passe. Argon2 est conçu pour être résistant aux attaques par matériel dédié,
     * en utilisant une grande quantité de mémoire et des opérations parallèles.
     *
     * @param password Mot de passe d'entrée
     * @param key_length Longueur de la clé à générer (en octets)
     * @param params Paramètres pour l'algorithme Argon2
     * @param variant Variante d'Argon2 à utiliser
     * @return La clé dérivée
     */
    static std::vector<unsigned char> argon2_kdf(
        const std::string& password,
        size_t key_length,
        const Argon2Params& params,
        Argon2Variant variant = Argon2Variant::Argon2id);

    /**
     * @brief Dérivation de clé avec HKDF (HMAC-based Key Derivation Function)
     *
     * Implémente HKDF selon RFC 5869 pour dériver une ou plusieurs clés à partir
     * d'un matériel de clé d'entrée. HKDF est particulièrement utile pour extraire
     * de l'entropie d'une source non uniforme et l'étendre à la taille souhaitée.
     *
     * @param input_key_material Matériel de clé d'entrée
     * @param salt Sel optionnel pour l'étape d'extraction
     * @param info Informations contextuelles pour l'étape d'expansion
     * @param output_length Longueur de la clé à générer
     * @param digest Algorithme de hachage à utiliser
     * @return La clé dérivée
     */
    static std::vector<unsigned char> hkdf(
        const std::vector<unsigned char>& input_key_material,
        const std::vector<unsigned char>& salt,
        const std::vector<unsigned char>& info,
        size_t output_length,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);

    /**
     * @brief Fonction haut niveau pour dériver une clé à partir d'un mot de passe
     *
     * Fournit une interface unifiée pour différents algorithmes de dérivation de clé.
     * Recommandations :
     * - Pour les mots de passe : Argon2
     * - Pour dériver des clés supplémentaires à partir d'une clé existante : HKDF
     * - Pour la compatibilité avec les systèmes existants : PBKDF2
     *
     * @param password Mot de passe ou clé d'entrée
     * @param salt Sel pour la dérivation
     * @param key_length Longueur de la clé à générer
     * @param algorithm Algorithme de dérivation à utiliser
     * @param iterations Nombre d'itérations (pour PBKDF2)
     * @param argon2_params Paramètres pour Argon2 (ignorés si un autre algorithme est utilisé)
     * @return La clé dérivée
     */
    static std::vector<unsigned char> derive_key(
        const std::string& password,
        const std::vector<unsigned char>& salt,
        size_t key_length,
        KdfAlgorithm algorithm = KdfAlgorithm::Argon2,
        int iterations = 10000,
        const Argon2Params& argon2_params = Argon2Params());

    /**
     * @brief Chiffrement intégré par courbe elliptique (ECIES)
     *
     * Implémente ECIES (Elliptic Curve Integrated Encryption Scheme), qui combine
     * cryptographie à clé publique par courbe elliptique et chiffrement symétrique
     * pour fournir un système hybride sécurisé.
     *
     * @param plaintext Données à chiffrer
     * @param recipient_public_key Clé publique du destinataire (PEM)
     * @param mode Mode d'opération ECIES
     * @param digest Algorithme de hachage à utiliser
     * @return Données chiffrées
     */
    static std::vector<unsigned char> ecies_encrypt(
        const std::vector<unsigned char>& plaintext,
        const std::string& recipient_public_key,
        ECIESMode mode = ECIESMode::AES_GCM,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);

    /**
     * @brief Déchiffrement ECIES
     *
     * Déchiffre les données chiffrées avec ECIES.
     *
     * @param ciphertext Données chiffrées
     * @param private_key Clé privée du destinataire (PEM)
     * @param mode Mode d'opération ECIES utilisé pour le chiffrement
     * @param digest Algorithme de hachage utilisé pour le chiffrement
     * @return Données déchiffrées ou vecteur vide en cas d'échec
     */
    static std::vector<unsigned char> ecies_decrypt(
        const std::vector<unsigned char>& ciphertext,
        const std::string& private_key,
        ECIESMode mode = ECIESMode::AES_GCM,
        DigestAlgorithm digest = DigestAlgorithm::SHA256);

    /**
     * @brief Chiffrement par enveloppe
     *
     * Implémente le chiffrement par enveloppe: une clé symétrique est générée,
     * utilisée pour chiffrer les données, puis elle-même chiffrée avec une clé publique.
     * Cette méthode est plus efficace qu'ECIES pour les grands volumes de données.
     *
     * @param plaintext Données à chiffrer
     * @param recipient_public_key Clé publique du destinataire (PEM)
     * @param algorithm Algorithme symétrique à utiliser
     * @param format Format de sortie des données chiffrées
     * @return Données chiffrées au format spécifié
     */
    static std::string envelope_encrypt(
        const std::vector<unsigned char>& plaintext,
        const std::string& recipient_public_key,
        SymmetricAlgorithm algorithm = SymmetricAlgorithm::AES_256_GCM,
        EnvelopeFormat format = EnvelopeFormat::BASE64);

    /**
     * @brief Déchiffrement par enveloppe
     *
     * Déchiffre les données chiffrées avec le chiffrement par enveloppe.
     *
     * @param ciphertext Données chiffrées
     * @param private_key Clé privée du destinataire (PEM)
     * @param format Format des données chiffrées
     * @return Données déchiffrées ou vecteur vide en cas d'échec
     */
    static std::vector<unsigned char> envelope_decrypt(
        const std::string& ciphertext,
        const std::string& private_key,
        EnvelopeFormat format = EnvelopeFormat::BASE64);

    /**
     * @brief Comparaison sécurisée de chaînes (résistante aux attaques par timing)
     *
     * Compare deux chaînes d'octets en temps constant pour éviter les fuites
     * d'information via des attaques par analyse de temps d'exécution.
     *
     * @param a Première chaîne
     * @param b Seconde chaîne
     * @return Vrai si les chaînes sont identiques, faux sinon
     */
    static bool constant_time_compare(
        const std::vector<unsigned char>& a,
        const std::vector<unsigned char>& b);

    /**
     * @brief Générateur de jetons authentifiés (encrypted token)
     *
     * Génère un jeton qui contient des informations authentifiées
     * et chiffrées. Utile pour créer des jetons d'authentification,
     * des identifiants de session, etc.
     *
     * @param payload Données à inclure dans le jeton
     * @param key Clé secrète pour le chiffrement
     * @param ttl Durée de validité du jeton en secondes (0 = pas d'expiration)
     * @return Jeton encodé en Base64URL
     */
    static std::string generate_token(
        const std::string& payload,
        const std::vector<unsigned char>& key,
        uint64_t ttl = 0);

    /**
     * @brief Vérification et décodage d'un jeton authentifié
     *
     * Vérifie et décode un jeton généré par generate_token.
     *
     * @param token Jeton à vérifier
     * @param key Clé secrète utilisée pour le chiffrement
     * @return Payload du jeton ou chaîne vide si le jeton est invalide ou expiré
     */
    static std::string verify_token(
        const std::string& token,
        const std::vector<unsigned char>& key);

    /**
     * @brief Encodage Base64URL
     *
     * Encode des données en Base64URL (variante de Base64 qui est
     * utilisable dans les URLs).
     *
     * @param data Données à encoder
     * @return Chaîne encodée en Base64URL
     */
    static std::string base64url_encode(const std::vector<unsigned char>& data);

    /**
     * @brief Décodage Base64URL
     *
     * Décode des données encodées en Base64URL.
     *
     * @param input Chaîne encodée en Base64URL
     * @return Données décodées
     */
    static std::vector<unsigned char> base64url_decode(const std::string& input);

    /**
     * @brief Génération de sel cryptographique
     *
     * Génère un sel aléatoire de la longueur spécifiée.
     *
     * @param length Longueur du sel en octets
     * @return Sel généré
     */
    static std::vector<unsigned char> generate_salt(size_t length = 16);

    /**
     * @brief Hachage sécurisé de mot de passe
     *
     * Hache un mot de passe de manière sécurisée en utilisant Argon2.
     * Cette fonction est conçue spécifiquement pour le stockage de mots
     * de passe et inclut directement le sel dans la sortie.
     *
     * @param password Mot de passe à hacher
     * @param variant Variante d'Argon2 à utiliser
     * @return Chaîne contenant le hash formaté avec paramètres
     */
    static std::string hash_password(
        const std::string& password,
        Argon2Variant variant = Argon2Variant::Argon2id);

    /**
     * @brief Vérification de mot de passe
     *
     * Vérifie si un mot de passe correspond à un hash généré par hash_password.
     *
     * @param password Mot de passe à vérifier
     * @param hash Hash à comparer
     * @return Vrai si le mot de passe correspond, faux sinon
     */
    static bool verify_password(
        const std::string& password,
        const std::string& hash);

    /**
     * @brief Génération de clés de signature Ed25519
     *
     * Génère une paire de clés de signature Ed25519, conçue 
     * pour être rapide et sécurisée.
     *
     * @return Paire de clés au format PEM (privée, publique)
     */
    static std::pair<std::string, std::string> generate_ed25519_keypair();

    /**
     * @brief Generate Ed25519 keypair returning raw byte vectors
     *
     * @return Paire de clés au format PEM (privée, publique)
     */
    static std::pair<std::vector<unsigned char>, std::vector<unsigned char>> 
    generate_ed25519_keypair_bytes();

    /**
     * @brief Sign data using Ed25519
     *
     * Signe des données avec l'algorithme Ed25519.
     *
     * @param data Données à signer
     * @param private_key Clé privée Ed25519 au format PEM
     * @return Signature
     */
    static std::vector<unsigned char> ed25519_sign(
        const std::vector<unsigned char>& data,
        const std::string& private_key);
    
    /**
     * @brief Sign data using Ed25519 with raw private key bytes
     *
     * Signe des données avec l'algorithme Ed25519.
     *
     * @param data Données à signer
     * @param private_key Clé privée Ed25519 au format PEM
     * @return Signature
     */
    static std::vector<unsigned char> ed25519_sign(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& private_key);

    /**
     * @brief Verify Ed25519 signature
     *
     * Vérifie une signature Ed25519.
     *
     * @param data Données qui ont été signées
     * @param signature Signature à vérifier
     * @param public_key Clé publique Ed25519 au format PEM
     * @return Vrai si la signature est valide
     */
    static bool ed25519_verify(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& signature,
        const std::string& public_key);
    
    /**
     * @brief Verify Ed25519 signature with raw public key bytes
     *
     * Vérifie une signature Ed25519.
     *
     * @param data Données qui ont été signées
     * @param signature Signature à vérifier
     * @param public_key Clé publique Ed25519 au format PEM
     * @return Vrai si la signature est valide
     */
    static bool ed25519_verify(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& signature,
        const std::vector<unsigned char>& public_key);

    /**
     * @brief Génération de clés X25519 pour échange de clés
     *
     * Génère une paire de clés X25519 pour l'échange de clés
     * Diffie-Hellman sur courbe elliptique.
     *
     * @return Paire de clés au format PEM (privée, publique)
     */
    static std::pair<std::string, std::string> generate_x25519_keypair();

    /**
     * @brief Generate X25519 keypair returning raw byte vectors
     *
     * Génère une paire de clés X25519 pour l'échange de clés
     * Diffie-Hellman sur courbe elliptique.
     *
     * @return Paire de clés au format PEM (privée, publique)
     */
    static std::pair<std::vector<unsigned char>, std::vector<unsigned char>> 
    generate_x25519_keypair_bytes();

    /**
     * @brief Échange de clés X25519
     *
     * Dérive un secret partagé à l'aide de l'algorithme X25519.
     *
     * @param private_key Clé privée X25519 au format PEM
     * @param peer_public_key Clé publique du pair au format PEM
     * @return Secret partagé
     */
    static std::vector<unsigned char> x25519_key_exchange(
        const std::string& private_key,
        const std::string& peer_public_key);
    
    /**
     * @brief Perform X25519 key exchange with raw key bytes
     *
     * Dérive un secret partagé à l'aide de l'algorithme X25519.
     *
     * @param private_key Clé privée X25519 au format PEM
     * @param peer_public_key Clé publique du pair au format PEM
     * @return Secret partagé
     */
    static std::vector<unsigned char> x25519_key_exchange(
        const std::vector<unsigned char>& private_key,
        const std::vector<unsigned char>& peer_public_key);

    /**
     * @brief Vecteur d'initialisation aléatoire pour une utilisation à usage unique
     *
     * Génère un vecteur d'initialisation aléatoire garanti unique, en incluant
     * un compteur et un timestamp pour éviter les doublons.
     *
     * @param size Taille du vecteur d'initialisation en octets
     * @return IV à usage unique
     */
    static std::vector<unsigned char> generate_unique_iv(size_t size = 12);

    /**
     * @brief Chiffrement authentifié de données avec authentification supplémentaire
     *
     * Chiffre des données avec authentification, en ajoutant des métadonnées
     * protégées par intégrité (ex: identifiant d'utilisateur, timestamp).
     * Les métadonnées sont incluses dans les AAD pour le chiffrement authentifié.
     *
     * @param plaintext Données à chiffrer
     * @param key Clé de chiffrement
     * @param metadata Métadonnées à protéger (non chiffrées mais authentifiées)
     * @param algorithm Algorithme à utiliser
     * @return Données chiffrées (avec IV, AAD et tag) au format structuré
     */
    static std::string encrypt_with_metadata(
        const std::vector<unsigned char>& plaintext,
        const std::vector<unsigned char>& key,
        const std::string& metadata,
        SymmetricAlgorithm algorithm = SymmetricAlgorithm::AES_256_GCM);

    /**
     * @brief Déchiffrement et vérification de l'intégrité des données et métadonnées
     *
     * Déchiffre des données protégées par encrypt_with_metadata et vérifie
     * l'intégrité des métadonnées.
     *
     * @param ciphertext Données chiffrées structurées
     * @param key Clé de déchiffrement
     * @param algorithm Algorithme utilisé pour le chiffrement
     * @return Structure contenant les données déchiffrées et les métadonnées vérifiées,
     *         ou valeur optionnelle vide si l'authentification échoue
     */
    static std::optional<std::pair<std::vector<unsigned char>, std::string>>
    decrypt_with_metadata(
        const std::string& ciphertext,
        const std::vector<unsigned char>& key,
        SymmetricAlgorithm algorithm = SymmetricAlgorithm::AES_256_GCM);

    // ECIES functions
    
    // ECIES encryption (using X25519 and AEAD)
    static std::pair<std::vector<unsigned char>, std::vector<unsigned char>> 
    ecies_encrypt(
        const std::vector<unsigned char>& data,
        const std::vector<unsigned char>& recipient_public_key,
        const std::vector<unsigned char>& optional_shared_info = {},
        ECIESMode mode = ECIESMode::AES_GCM);
    
    // ECIES decryption
    static std::vector<unsigned char> 
    ecies_decrypt(
        const std::vector<unsigned char>& encrypted_data,
        const std::vector<unsigned char>& ephemeral_public_key,
        const std::vector<unsigned char>& recipient_private_key,
        const std::vector<unsigned char>& optional_shared_info = {},
        ECIESMode mode = ECIESMode::AES_GCM);
};
} // namespace qb

#endif // QB_IO_CRYPTO_H
