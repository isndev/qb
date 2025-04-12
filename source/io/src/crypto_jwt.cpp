/**
 * @file qb/source/io/src/crypto_jwt.cpp
 * @brief Implementation of JWT (JSON Web Token) functionality
 * 
 * This file implements the JWT functions defined in crypto_jwt.h.
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
 */

#include <qb/io/crypto_jwt.h>
#include <qb/json.h>
#include <sstream>
#include <stdexcept>

namespace qb {

// Convert base64url to standard base64
std::string base64url_to_base64(const std::string& base64url) {
    std::string base64 = base64url;
    
    // Replace URL-safe characters with standard Base64 characters
    std::replace(base64.begin(), base64.end(), '-', '+');
    std::replace(base64.begin(), base64.end(), '_', '/');
    
    // Add padding if needed
    switch (base64.length() % 4) {
        case 0: break;                    // No padding needed
        case 2: base64.append("=="); break; // Add two padding characters
        case 3: base64.append("="); break;  // Add one padding character
        default: throw std::runtime_error("Invalid base64url string");
    }
    
    return base64;
}

// Convert standard base64 to base64url
std::string base64_to_base64url(const std::string& base64) {
    std::string base64url = base64;
    
    // Replace standard Base64 characters with URL-safe characters
    std::replace(base64url.begin(), base64url.end(), '+', '-');
    std::replace(base64url.begin(), base64url.end(), '/', '_');
    
    // Remove padding
    size_t pad_pos = base64url.find_first_of('=');
    if (pad_pos != std::string::npos) {
        base64url.erase(pad_pos);
    }
    
    return base64url;
}

std::vector<unsigned char> jwt::sign_data(const std::string& data, const CreateOptions& options) {
    // Convert string key to bytes for HMAC algorithms
    std::vector<unsigned char> key_bytes;
    if (options.algorithm == Algorithm::HS256 || 
        options.algorithm == Algorithm::HS384 || 
        options.algorithm == Algorithm::HS512) {
        key_bytes.assign(options.key.begin(), options.key.end());
    }
    
    std::vector<unsigned char> data_bytes(data.begin(), data.end());
    
    // Sign based on algorithm
    switch (options.algorithm) {
        case Algorithm::HS256:
        case Algorithm::HS384:
        case Algorithm::HS512:
            return crypto::hmac(data_bytes, key_bytes, get_digest_algorithm(options.algorithm));
            
        case Algorithm::RS256:
        case Algorithm::RS384:
        case Algorithm::RS512:
            return crypto::rsa_sign(data_bytes, options.key, get_digest_algorithm(options.algorithm));
            
        case Algorithm::ES256:
        case Algorithm::ES384:
        case Algorithm::ES512:
            return crypto::ec_sign(data_bytes, options.key, get_digest_algorithm(options.algorithm));
            
        case Algorithm::EdDSA:
            return crypto::ed25519_sign(data_bytes, options.key);
            
        default:
            throw std::runtime_error("Unsupported algorithm");
    }
}

bool jwt::verify_signature(const std::string& data, 
                           const std::vector<unsigned char>& signature, 
                           const VerifyOptions& options) {
    // Convert string key to bytes for HMAC algorithms
    std::vector<unsigned char> key_bytes;
    if (options.algorithm == Algorithm::HS256 || 
        options.algorithm == Algorithm::HS384 || 
        options.algorithm == Algorithm::HS512) {
        key_bytes.assign(options.key.begin(), options.key.end());
    }
    
    std::vector<unsigned char> data_bytes(data.begin(), data.end());
    
    // Verify based on algorithm
    switch (options.algorithm) {
        case Algorithm::HS256:
        case Algorithm::HS384:
        case Algorithm::HS512: {
            auto expected = crypto::hmac(data_bytes, key_bytes, get_digest_algorithm(options.algorithm));
            return crypto::constant_time_compare(signature, expected);
        }
            
        case Algorithm::RS256:
        case Algorithm::RS384:
        case Algorithm::RS512:
            return crypto::rsa_verify(data_bytes, signature, options.key, 
                                      get_digest_algorithm(options.algorithm));
            
        case Algorithm::ES256:
        case Algorithm::ES384:
        case Algorithm::ES512:
            return crypto::ec_verify(data_bytes, signature, options.key, 
                                     get_digest_algorithm(options.algorithm));
            
        case Algorithm::EdDSA:
            return crypto::ed25519_verify(data_bytes, signature, options.key);
            
        default:
            return false;
    }
}

crypto::DigestAlgorithm jwt::get_digest_algorithm(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::HS256:
        case Algorithm::RS256:
        case Algorithm::ES256:
            return crypto::DigestAlgorithm::SHA256;
            
        case Algorithm::HS384:
        case Algorithm::RS384:
        case Algorithm::ES384:
            return crypto::DigestAlgorithm::SHA384;
            
        case Algorithm::HS512:
        case Algorithm::RS512:
        case Algorithm::ES512:
            return crypto::DigestAlgorithm::SHA512;
            
        case Algorithm::EdDSA:
            // EdDSA doesn't use a separate digest algorithm in the same way
            return crypto::DigestAlgorithm::SHA512; // Default for internal use
            
        default:
            return crypto::DigestAlgorithm::SHA256;
    }
}

int64_t jwt::current_timestamp() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

std::string jwt::algorithm_to_string(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::HS256: return "HS256";
        case Algorithm::HS384: return "HS384";
        case Algorithm::HS512: return "HS512";
        case Algorithm::RS256: return "RS256";
        case Algorithm::RS384: return "RS384";
        case Algorithm::RS512: return "RS512";
        case Algorithm::ES256: return "ES256";
        case Algorithm::ES384: return "ES384";
        case Algorithm::ES512: return "ES512";
        case Algorithm::EdDSA: return "EdDSA";
        default: return "unknown";
    }
}

std::optional<jwt::Algorithm> jwt::algorithm_from_string(const std::string& algorithm_str) {
    if (algorithm_str == "HS256") return Algorithm::HS256;
    if (algorithm_str == "HS384") return Algorithm::HS384;
    if (algorithm_str == "HS512") return Algorithm::HS512;
    if (algorithm_str == "RS256") return Algorithm::RS256;
    if (algorithm_str == "RS384") return Algorithm::RS384;
    if (algorithm_str == "RS512") return Algorithm::RS512;
    if (algorithm_str == "ES256") return Algorithm::ES256;
    if (algorithm_str == "ES384") return Algorithm::ES384;
    if (algorithm_str == "ES512") return Algorithm::ES512;
    if (algorithm_str == "EdDSA") return Algorithm::EdDSA;
    
    return std::nullopt;
}

std::string jwt::create(const std::map<std::string, std::string>& payload, 
                       const CreateOptions& options) {
    // Create header
    json header_json;
    header_json["alg"] = algorithm_to_string(options.algorithm);
    
    if (options.type.has_value()) {
        header_json["typ"] = options.type.value();
    }
    
    if (options.content_type.has_value()) {
        header_json["cty"] = options.content_type.value();
    }
    
    if (options.key_id.has_value()) {
        header_json["kid"] = options.key_id.value();
    }
    
    // Add any additional header claims
    for (const auto& [key, value] : options.header_claims) {
        header_json[key] = value;
    }
    
    // Create payload
    json payload_json;
    for (const auto& [key, value] : payload) {
        payload_json[key] = value;
    }
    
    // Base64Url encode header and payload
    std::string header_str = header_json.dump();
    std::string payload_str = payload_json.dump();
    
    std::string header_b64 = base64_to_base64url(
        crypto::base64::encode(header_str)
    );
    
    std::string payload_b64 = base64_to_base64url(
        crypto::base64::encode(payload_str)
    );
    
    // Create signature
    std::string signature_data = header_b64 + "." + payload_b64;
    auto signature = sign_data(signature_data, options);
    
    // Base64Url encode signature
    std::string signature_b64 = base64_to_base64url(
        crypto::base64::encode(std::string(signature.begin(), signature.end()))
    );
    
    // Combine to create JWT
    return header_b64 + "." + payload_b64 + "." + signature_b64;
}

std::string jwt::create_token(
    const std::map<std::string, std::string>& payload,
    const std::string& issuer,
    const std::string& subject,
    const std::string& audience,
    std::chrono::seconds expires_in,
    std::chrono::seconds not_before,
    const std::string& jti,
    const CreateOptions& options) {
    
    // Start with custom payload
    std::map<std::string, std::string> token_payload = payload;
    
    // Add standard claims
    int64_t current_time = current_timestamp();
    
    // Issued at
    token_payload["iat"] = std::to_string(current_time);
    
    // Expiration time
    if (expires_in.count() > 0) {
        token_payload["exp"] = std::to_string(current_time + expires_in.count());
    }
    
    // Not before
    if (not_before.count() > 0) {
        token_payload["nbf"] = std::to_string(current_time + not_before.count());
    }
    
    // Issuer
    if (!issuer.empty()) {
        token_payload["iss"] = issuer;
    }
    
    // Subject
    if (!subject.empty()) {
        token_payload["sub"] = subject;
    }
    
    // Audience
    if (!audience.empty()) {
        token_payload["aud"] = audience;
    }
    
    // JWT ID
    if (!jti.empty()) {
        token_payload["jti"] = jti;
    }
    
    return create(token_payload, options);
}

jwt::TokenParts jwt::decode(const std::string& token) {
    // Split token into parts
    std::vector<std::string> parts;
    std::stringstream ss(token);
    std::string part;
    
    while (std::getline(ss, part, '.')) {
        parts.push_back(part);
    }
    
    // JWT should have 3 parts: header, payload, signature
    if (parts.size() != 3) {
        throw std::runtime_error("Invalid JWT format");
    }
    
    TokenParts token_parts;
    
    // Decode header
    try {
        std::string header_b64 = base64url_to_base64(parts[0]);
        token_parts.header = crypto::base64::decode(header_b64);
    } catch (const std::exception& e) {
        throw std::runtime_error("Invalid JWT header encoding");
    }
    
    // Decode payload
    try {
        std::string payload_b64 = base64url_to_base64(parts[1]);
        token_parts.payload = crypto::base64::decode(payload_b64);
    } catch (const std::exception& e) {
        throw std::runtime_error("Invalid JWT payload encoding");
    }
    
    // Store signature (still base64url encoded)
    token_parts.signature = parts[2];
    
    return token_parts;
}

jwt::ValidationResult jwt::verify(const std::string& token, const VerifyOptions& options) {
    try {
        // Split token
        std::vector<std::string> parts;
        std::stringstream ss(token);
        std::string part;
        
        while (std::getline(ss, part, '.')) {
            parts.push_back(part);
        }
        
        // JWT should have 3 parts: header, payload, signature
        if (parts.size() != 3) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        // Keep the raw encoded parts for signature verification
        std::string header_b64url = parts[0];
        std::string payload_b64url = parts[1];
        std::string signature_b64url = parts[2];
        
        // Decode header
        std::string header_b64 = base64url_to_base64(header_b64url);
        std::string header_str;
        try {
            header_str = crypto::base64::decode(header_b64);
        } catch (const std::exception&) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        // Parse header
        json header_json;
        try {
            header_json = json::parse(header_str);
        } catch (const std::exception&) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        // Verify algorithm
        if (!header_json.contains("alg")) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        std::string alg_str = header_json["alg"].get<std::string>();
        auto alg = algorithm_from_string(alg_str);
        
        if (!alg.has_value() || alg.value() != options.algorithm) {
            return ValidationResult(ValidationError::INVALID_SIGNATURE);
        }
        
        // Decode payload
        std::string payload_b64 = base64url_to_base64(payload_b64url);
        std::string payload_str;
        try {
            payload_str = crypto::base64::decode(payload_b64);
        } catch (const std::exception&) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        // Parse payload
        json payload_json;
        try {
            payload_json = json::parse(payload_str);
        } catch (const std::exception&) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        // Verify signature
        std::string signature_data = header_b64url + "." + payload_b64url;
        std::string signature_b64 = base64url_to_base64(signature_b64url);
        std::string signature_str;
        
        try {
            signature_str = crypto::base64::decode(signature_b64);
        } catch (const std::exception&) {
            return ValidationResult(ValidationError::INVALID_FORMAT);
        }
        
        std::vector<unsigned char> signature(signature_str.begin(), signature_str.end());
        
        if (!verify_signature(signature_data, signature, options)) {
            return ValidationResult(ValidationError::INVALID_SIGNATURE);
        }
        
        // Get current time
        int64_t current_time = current_timestamp();
        int64_t skew = options.clock_skew.count();
        
        // Verify expiration time
        if (options.verify_expiration && payload_json.contains("exp")) {
            int64_t exp = std::stoll(payload_json["exp"].get<std::string>());
            if (current_time > exp + skew) {
                return ValidationResult(ValidationError::TOKEN_EXPIRED);
            }
        }
        
        // Verify not before time
        if (options.verify_not_before && payload_json.contains("nbf")) {
            int64_t nbf = std::stoll(payload_json["nbf"].get<std::string>());
            if (current_time < nbf - skew) {
                return ValidationResult(ValidationError::TOKEN_NOT_ACTIVE);
            }
        }
        
        // Verify issuer
        if (options.verify_issuer && options.issuer.has_value()) {
            if (!payload_json.contains("iss") || 
                payload_json["iss"].get<std::string>() != options.issuer.value()) {
                return ValidationResult(ValidationError::INVALID_ISSUER);
            }
        }
        
        // Verify audience
        if (options.verify_audience && options.audience.has_value()) {
            if (!payload_json.contains("aud") || 
                payload_json["aud"].get<std::string>() != options.audience.value()) {
                return ValidationResult(ValidationError::INVALID_AUDIENCE);
            }
        }
        
        // Verify subject
        if (options.verify_subject && options.subject.has_value()) {
            if (!payload_json.contains("sub") || 
                payload_json["sub"].get<std::string>() != options.subject.value()) {
                return ValidationResult(ValidationError::INVALID_SUBJECT);
            }
        }
        
        // Verify JWT ID
        if (options.verify_jti && options.jti.has_value()) {
            if (!payload_json.contains("jti") || 
                payload_json["jti"].get<std::string>() != options.jti.value()) {
                return ValidationResult(ValidationError::CLAIM_MISMATCH);
            }
        }
        
        // Verify required claims
        for (const auto& [claim_name, claim_value] : options.required_claims) {
            if (!payload_json.contains(claim_name) || 
                payload_json[claim_name].get<std::string>() != claim_value) {
                return ValidationResult(ValidationError::CLAIM_MISMATCH);
            }
        }
        
        // Extract payload as map
        ValidationResult result;
        for (const auto& [key, value] : payload_json.items()) {
            result.payload[key] = value.get<std::string>();
        }
        
        return result;
        
    } catch (const std::exception&) {
        return ValidationResult(ValidationError::INVALID_FORMAT);
    }
}

} // namespace qb 