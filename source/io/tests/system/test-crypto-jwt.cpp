/**
 * @file qb/source/io/tests/system/test-crypto-jwt.cpp
 * @brief Tests for JWT functionality
 * 
 * This file contains tests for the JWT functionality implemented in crypto_jwt.h/cpp.
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

#include <gtest/gtest.h>
#include <qb/io/crypto_jwt.h>
#include <thread>
#include <fstream>

using namespace qb;

// Helper function to read file contents
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Test basic JWT creation and verification with HMAC
TEST(CryptoJWT, BasicHmacToken) {
    // Create a token with HMAC-SHA256
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"},
        {"username", "testuser"},
        {"role", "admin"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create(payload, create_options);
    
    // Token should have 3 parts separated by dots
    ASSERT_NE(token.find_first_of('.'), std::string::npos);
    ASSERT_NE(token.find_last_of('.'), token.find_first_of('.'));
    
    // Verify the token
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");
    ASSERT_EQ(result.payload.at("username"), "testuser");
    ASSERT_EQ(result.payload.at("role"), "admin");
}

// Test token with standard claims
TEST(CryptoJWT, StandardClaims) {
    // Create a token with standard claims
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create_token(
        payload,             // Custom payload
        "test-issuer",       // Issuer
        "user-12345",        // Subject
        "test-audience",     // Audience
        std::chrono::hours(1), // Expires in 1 hour
        std::chrono::seconds(0), // Valid immediately
        "token-id-123",      // JWT ID
        create_options
    );
    
    // Verify the token with claim checks
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    verify_options.verify_issuer = true;
    verify_options.issuer = "test-issuer";
    verify_options.verify_audience = true;
    verify_options.audience = "test-audience";
    verify_options.verify_subject = true;
    verify_options.subject = "user-12345";
    verify_options.verify_jti = true;
    verify_options.jti = "token-id-123";
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");
    ASSERT_EQ(result.payload.at("iss"), "test-issuer");
    ASSERT_EQ(result.payload.at("sub"), "user-12345");
    ASSERT_EQ(result.payload.at("aud"), "test-audience");
    ASSERT_EQ(result.payload.at("jti"), "token-id-123");
    ASSERT_TRUE(result.payload.find("iat") != result.payload.end());
    ASSERT_TRUE(result.payload.find("exp") != result.payload.end());
}

// Test token expiration
TEST(CryptoJWT, Expiration) {
    // Create a token that expires in 1 second
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create_token(
        payload,
        "test-issuer",
        "user-12345",
        "test-audience",
        std::chrono::seconds(1), // Expires in 1 second
        std::chrono::seconds(0),
        "token-id-123",
        create_options
    );
    
    // Verify immediately - should be valid
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    verify_options.verify_expiration = true;
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    
    // Wait for expiration
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Verify again - should be expired
    auto result2 = jwt::verify(token, verify_options);
    ASSERT_FALSE(result2.is_valid());
    ASSERT_EQ(result2.error, jwt::ValidationError::TOKEN_EXPIRED);
    
    // Verify with clock skew - should be valid again
    verify_options.clock_skew = std::chrono::seconds(5);
    auto result3 = jwt::verify(token, verify_options);
    ASSERT_TRUE(result3.is_valid());
}

// Test signature validation
TEST(CryptoJWT, SignatureValidation) {
    // Create a valid token
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create(payload, create_options);
    
    // Verify with correct key
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    
    // Verify with incorrect key
    verify_options.key = "wrong-secret-key";
    auto result2 = jwt::verify(token, verify_options);
    ASSERT_FALSE(result2.is_valid());
    ASSERT_EQ(result2.error, jwt::ValidationError::INVALID_SIGNATURE);
}

// Test token that is not yet valid
TEST(CryptoJWT, NotBeforeValidation) {
    // Create a token that is not valid for 2 seconds
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create_token(
        payload,
        "",
        "",
        "",
        std::chrono::hours(1),
        std::chrono::seconds(2), // Not valid before 2 seconds
        "",
        create_options
    );
    
    // Verify immediately - should be invalid
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    verify_options.verify_not_before = true;
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_FALSE(result.is_valid());
    ASSERT_EQ(result.error, jwt::ValidationError::TOKEN_NOT_ACTIVE);
    
    // Wait for token to become valid
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Verify again - should be valid
    auto result2 = jwt::verify(token, verify_options);
    ASSERT_TRUE(result2.is_valid());
}

// Test asymmetric algorithms with RSA keys
TEST(CryptoJWT, RSASignature) {
    // For RSA tests, we need actual RSA keys
    // Replace these paths with actual key paths for testing
    std::string keys_path = "/Users/mbelhadi/Repos/qb-auth-project/temp_keys/";
    
    try {
        std::string rsa_private_key = read_file(keys_path + "rsa_private.pem");
        std::string rsa_public_key = read_file(keys_path + "rsa_public.pem");
        
        // Create a token with RSA-SHA256
        std::map<std::string, std::string> payload = {
            {"user_id", "12345"}
        };
        
        jwt::CreateOptions create_options;
        create_options.algorithm = jwt::Algorithm::RS256;
        create_options.key = rsa_private_key;
        
        std::string token = jwt::create(payload, create_options);
        
        // Verify with public key
        jwt::VerifyOptions verify_options;
        verify_options.algorithm = jwt::Algorithm::RS256;
        verify_options.key = rsa_public_key;
        
        auto result = jwt::verify(token, verify_options);
        ASSERT_TRUE(result.is_valid());
        ASSERT_EQ(result.payload.at("user_id"), "12345");
        
    } catch (const std::exception& e) {
        std::cerr << "Skipping RSA test due to missing keys: " << e.what() << std::endl;
        GTEST_SKIP();
    }
}

// Test ECDSA signatures
TEST(CryptoJWT, ECDSASignature) {
    // For ECDSA tests, we need actual EC keys
    // Replace these paths with actual key paths for testing
    std::string keys_path = "/Users/mbelhadi/Repos/qb-auth-project/temp_keys/";
    
    try {
        std::string ec_private_key = read_file(keys_path + "ec_private.pem");
        std::string ec_public_key = read_file(keys_path + "ec_public.pem");
        
        // Create a token with ES256
        std::map<std::string, std::string> payload = {
            {"user_id", "12345"}
        };
        
        jwt::CreateOptions create_options;
        create_options.algorithm = jwt::Algorithm::ES256;
        create_options.key = ec_private_key;
        
        std::string token = jwt::create(payload, create_options);
        
        // Verify with public key
        jwt::VerifyOptions verify_options;
        verify_options.algorithm = jwt::Algorithm::ES256;
        verify_options.key = ec_public_key;
        
        auto result = jwt::verify(token, verify_options);
        ASSERT_TRUE(result.is_valid());
        ASSERT_EQ(result.payload.at("user_id"), "12345");
        
    } catch (const std::exception& e) {
        std::cerr << "Skipping ECDSA test due to missing keys: " << e.what() << std::endl;
        GTEST_SKIP();
    }
}

// Test EdDSA signatures
TEST(CryptoJWT, EdDSASignature) {
    // For EdDSA tests, we need actual Ed25519 keys
    // Replace these paths with actual key paths for testing
    std::string keys_path = "/Users/mbelhadi/Repos/qb-auth-project/temp_keys/";
    
    try {
        std::string ed25519_private_key = read_file(keys_path + "ed25519_private.pem");
        std::string ed25519_public_key = read_file(keys_path + "ed25519_public.pem");
        
        // Create a token with EdDSA
        std::map<std::string, std::string> payload = {
            {"user_id", "12345"}
        };
        
        jwt::CreateOptions create_options;
        create_options.algorithm = jwt::Algorithm::EdDSA;
        create_options.key = ed25519_private_key;
        
        std::string token = jwt::create(payload, create_options);
        
        // Verify with public key
        jwt::VerifyOptions verify_options;
        verify_options.algorithm = jwt::Algorithm::EdDSA;
        verify_options.key = ed25519_public_key;
        
        auto result = jwt::verify(token, verify_options);
        ASSERT_TRUE(result.is_valid());
        ASSERT_EQ(result.payload.at("user_id"), "12345");
        
    } catch (const std::exception& e) {
        std::cerr << "Skipping EdDSA test due to missing keys: " << e.what() << std::endl;
        GTEST_SKIP();
    }
}

// Test custom claim validation
TEST(CryptoJWT, CustomClaimValidation) {
    // Create a token with custom claims
    std::map<std::string, std::string> payload = {
        {"user_id", "12345"},
        {"role", "admin"},
        {"organization", "test-org"}
    };
    
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key = "your-secret-key";
    
    std::string token = jwt::create(payload, create_options);
    
    // Verify with required custom claims
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key = "your-secret-key";
    verify_options.required_claims = {
        {"role", "admin"},
        {"organization", "test-org"}
    };
    
    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    
    // Verify with incorrect required claim
    verify_options.required_claims = {
        {"role", "user"} // Should be "admin"
    };
    
    auto result2 = jwt::verify(token, verify_options);
    ASSERT_FALSE(result2.is_valid());
    ASSERT_EQ(result2.error, jwt::ValidationError::CLAIM_MISMATCH);
    
    // Verify with missing required claim
    verify_options.required_claims = {
        {"department", "engineering"} // This claim doesn't exist
    };
    
    auto result3 = jwt::verify(token, verify_options);
    ASSERT_FALSE(result3.is_valid());
    ASSERT_EQ(result3.error, jwt::ValidationError::CLAIM_MISMATCH);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 