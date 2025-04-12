/**
 * @file qb/io/crypto_jwt.h
 * @brief JWT (JSON Web Token) implementation for the QB IO library
 *
 * This file provides a complete JWT implementation supporting various
 * signing algorithms (HMAC, RSA, ECDSA, EdDSA) and standard claims validation.
 * It follows RFC 7519 specification for JSON Web Tokens.
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

#ifndef QB_IO_CRYPTO_JWT_H
#define QB_IO_CRYPTO_JWT_H

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "crypto.h"

namespace qb {

/**
 * @class jwt
 * @brief Comprehensive JWT implementation for the QB IO library
 *
 * This class provides functionality for creating, signing, and verifying
 * JSON Web Tokens (JWT) according to RFC 7519. It supports multiple
 * signing algorithms and standard JWT claims validation.
 */
class jwt {
public:
    /**
     * @brief Supported JWT signing algorithms
     */
    enum class Algorithm {
        HS256, // HMAC using SHA-256
        HS384, // HMAC using SHA-384
        HS512, // HMAC using SHA-512
        RS256, // RSASSA-PKCS1-v1_5 using SHA-256
        RS384, // RSASSA-PKCS1-v1_5 using SHA-384
        RS512, // RSASSA-PKCS1-v1_5 using SHA-512
        ES256, // ECDSA using P-256 and SHA-256
        ES384, // ECDSA using P-384 and SHA-384
        ES512, // ECDSA using P-521 and SHA-512
        EdDSA  // Edwards-curve Digital Signature Algorithm (Ed25519)
    };

    /**
     * @brief JWT validation error codes
     */
    enum class ValidationError {
        NONE,
        INVALID_FORMAT,
        INVALID_SIGNATURE,
        TOKEN_EXPIRED,
        TOKEN_NOT_ACTIVE,
        INVALID_ISSUER,
        INVALID_AUDIENCE,
        INVALID_SUBJECT,
        CLAIM_MISMATCH
    };

    /**
     * @brief Result of JWT validation containing error code and payload if valid
     */
    struct ValidationResult {
        ValidationError error;
        std::map<std::string, std::string> payload;
        
        bool is_valid() const { return error == ValidationError::NONE; }
        
        ValidationResult() : error(ValidationError::NONE) {}
        ValidationResult(ValidationError err) : error(err) {}
    };

    /**
     * @brief JWT token parts
     */
    struct TokenParts {
        std::string header;
        std::string payload;
        std::string signature;
    };
    
    /**
     * @brief JWT creation options
     */
    struct CreateOptions {
        Algorithm algorithm;
        std::string key;              // Secret key for HMAC, or private key for asymmetric algorithms
        std::optional<std::string> type; // Token type
        std::optional<std::string> content_type; // Optional content type
        std::optional<std::string> key_id;       // Optional key ID
        std::map<std::string, std::string> header_claims; // Additional header claims
        
        CreateOptions() : algorithm(Algorithm::HS256), type("JWT") {}
    };
    
    /**
     * @brief JWT verification options
     */
    struct VerifyOptions {
        Algorithm algorithm;
        std::string key;              // Secret key for HMAC, or public key for asymmetric algorithms
        bool verify_expiration;
        bool verify_not_before;
        bool verify_issuer;
        std::optional<std::string> issuer;
        bool verify_audience;
        std::optional<std::string> audience;
        bool verify_subject;
        std::optional<std::string> subject;
        bool verify_jti;
        std::optional<std::string> jti;
        std::chrono::seconds clock_skew;
        std::map<std::string, std::string> required_claims; // Additional required claims
        
        VerifyOptions() 
            : algorithm(Algorithm::HS256),
              verify_expiration(true),
              verify_not_before(true),
              verify_issuer(false),
              verify_audience(false),
              verify_subject(false),
              verify_jti(false),
              clock_skew(std::chrono::seconds(0)) {}
    };

    /**
     * @brief Create a JWT token with custom payload and options
     *
     * @param payload Map of claims to include in the payload
     * @param options Options for creating the token
     * @return JWT token string
     */
    static std::string create(const std::map<std::string, std::string>& payload, 
                             const CreateOptions& options);
    
    /**
     * @brief Create a JWT token with standard claims and custom payload
     *
     * @param payload Map of custom claims to include in the payload
     * @param issuer Token issuer
     * @param subject Token subject
     * @param audience Token audience
     * @param expires_in Token expiration time in seconds from now
     * @param not_before Token not valid before time in seconds from now (default: 0)
     * @param jti Unique JWT ID
     * @param options Options for creating the token
     * @return JWT token string
     */
    static std::string create_token(
        const std::map<std::string, std::string>& payload,
        const std::string& issuer,
        const std::string& subject,
        const std::string& audience,
        std::chrono::seconds expires_in,
        std::chrono::seconds not_before = std::chrono::seconds(0),
        const std::string& jti = "",
        const CreateOptions& options = CreateOptions());
    
    /**
     * @brief Verify a JWT token
     *
     * @param token JWT token to verify
     * @param options Options for verifying the token
     * @return ValidationResult containing error code and payload if valid
     */
    static ValidationResult verify(const std::string& token, const VerifyOptions& options);
    
    /**
     * @brief Decode a JWT token without verification
     *
     * @param token JWT token to decode
     * @return TokenParts containing header, payload, and signature
     * @throws std::runtime_error if token format is invalid
     */
    static TokenParts decode(const std::string& token);
    
    /**
     * @brief Get string representation of algorithm
     *
     * @param algorithm Algorithm to convert to string
     * @return String representation of algorithm
     */
    static std::string algorithm_to_string(Algorithm algorithm);
    
    /**
     * @brief Get algorithm from string representation
     *
     * @param algorithm_str String representation of algorithm
     * @return Algorithm enum value, or std::nullopt if not recognized
     */
    static std::optional<Algorithm> algorithm_from_string(const std::string& algorithm_str);

private:
    /**
     * @brief Sign data using the specified algorithm
     *
     * @param data Data to sign
     * @param options Options containing algorithm and key
     * @return Signature bytes
     */
    static std::vector<unsigned char> sign_data(const std::string& data, 
                                               const CreateOptions& options);
    
    /**
     * @brief Verify signature using the specified algorithm
     *
     * @param data Data that was signed
     * @param signature Signature bytes
     * @param options Options containing algorithm and key
     * @return true if signature is valid, false otherwise
     */
    static bool verify_signature(const std::string& data,
                                const std::vector<unsigned char>& signature,
                                const VerifyOptions& options);
    
    /**
     * @brief Convert crypto::DigestAlgorithm to corresponding algorithm for the signing method
     *
     * @param algorithm JWT algorithm
     * @return Corresponding crypto::DigestAlgorithm
     */
    static crypto::DigestAlgorithm get_digest_algorithm(Algorithm algorithm);
    
    /**
     * @brief Get current Unix timestamp
     *
     * @return Current Unix timestamp in seconds
     */
    static int64_t current_timestamp();
};

} // namespace qb

#endif // QB_IO_CRYPTO_JWT_H 