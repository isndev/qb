/**
 * @file qb/source/io/tests/system/test-crypto-jwt.cpp
 * @brief Tests for JWT functionality
 *
 * This file contains tests for the JWT functionality implemented in crypto_jwt.h/cpp.
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
 */

#include <gtest/gtest.h>
#include <qb/io/crypto.h>
#include <qb/io/crypto_jwt.h>
#include <algorithm>
#include <fstream>
#include <thread>
#include <tuple>
#include <vector>

using namespace qb;

// Helper function to read file contents
std::string
read_file(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string
to_base64url(const std::string &data) {
    std::string encoded = crypto::base64::encode(data);
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    const auto padding = encoded.find('=');
    if (padding != std::string::npos) {
        encoded.erase(padding);
    }
    return encoded;
}

// Test basic JWT creation and verification with HMAC
TEST(CryptoJWT, BasicHmacToken) {
    // Create a token with HMAC-SHA256
    std::map<std::string, std::string> payload = {{"user_id", "12345"}, {"username", "testuser"}, {"role", "admin"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create(payload, create_options);

    // Token should have 3 parts separated by dots
    ASSERT_NE(token.find_first_of('.'), std::string::npos);
    ASSERT_NE(token.find_last_of('.'), token.find_first_of('.'));

    // Verify the token
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "your-secret-key";

    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");
    ASSERT_EQ(result.payload.at("username"), "testuser");
    ASSERT_EQ(result.payload.at("role"), "admin");
}

// Test token with standard claims
TEST(CryptoJWT, StandardClaims) {
    // Create a token with standard claims
    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create_token(payload,                 // Custom payload
                                          "test-issuer",           // Issuer
                                          "user-12345",            // Subject
                                          "test-audience",         // Audience
                                          std::chrono::hours(1),   // Expires in 1 hour
                                          std::chrono::seconds(0), // Valid immediately
                                          "token-id-123",          // JWT ID
                                          create_options);

    // Verify the token with claim checks
    jwt::VerifyOptions verify_options;
    verify_options.algorithm       = jwt::Algorithm::HS256;
    verify_options.key             = "your-secret-key";
    verify_options.verify_issuer   = true;
    verify_options.issuer          = "test-issuer";
    verify_options.verify_audience = true;
    verify_options.audience        = "test-audience";
    verify_options.verify_subject  = true;
    verify_options.subject         = "user-12345";
    verify_options.verify_jti      = true;
    verify_options.jti             = "token-id-123";

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
    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create_token(payload, "test-issuer", "user-12345", "test-audience",
                                          std::chrono::seconds(1), // Expires in 1 second
                                          std::chrono::seconds(0), "token-id-123", create_options);

    // Verify immediately - should be valid
    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "your-secret-key";
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
    auto result3              = jwt::verify(token, verify_options);
    ASSERT_TRUE(result3.is_valid());
}

// Test signature validation
TEST(CryptoJWT, SignatureValidation) {
    // Create a valid token
    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create(payload, create_options);

    // Verify with correct key
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "your-secret-key";

    auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());

    // Verify with incorrect key
    verify_options.key = "wrong-secret-key";
    auto result2       = jwt::verify(token, verify_options);
    ASSERT_FALSE(result2.is_valid());
    ASSERT_EQ(result2.error, jwt::ValidationError::INVALID_SIGNATURE);
}

// Test token that is not yet valid
TEST(CryptoJWT, NotBeforeValidation) {
    // Create a token that is not valid for 2 seconds
    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create_token(payload, "", "", "", std::chrono::hours(1),
                                          std::chrono::seconds(2), // Not valid before 2 seconds
                                          "", create_options);

    // Verify immediately - should be invalid
    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "your-secret-key";
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

// Test asymmetric algorithms with RSA keys.
// Keys are generated in-process so the RS256 sign+verify path is always exercised
// (the test previously read keys from a hardcoded developer-local path and silently
// GTEST_SKIP()'d everywhere, leaving asymmetric JWT verification untested).
TEST(CryptoJWT, RSASignature) {
    auto [rsa_private_key, rsa_public_key] = crypto::generate_rsa_keypair(2048);
    ASSERT_FALSE(rsa_private_key.empty());
    ASSERT_FALSE(rsa_public_key.empty());

    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::RS256;
    create_options.key       = rsa_private_key;
    std::string token        = jwt::create(payload, create_options);

    // Verify with public key
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::RS256;
    verify_options.key       = rsa_public_key;
    auto result              = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");

    // Negative: a token signed by a different key must NOT verify.
    auto [other_private, other_public] = crypto::generate_rsa_keypair(2048);
    verify_options.key                 = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
}

// Test ECDSA signatures (ES256 = P-256 / prime256v1). Keys generated in-process.
TEST(CryptoJWT, ECDSASignature) {
    auto [ec_private_key, ec_public_key] = crypto::generate_ec_keypair("prime256v1");
    ASSERT_FALSE(ec_private_key.empty());
    ASSERT_FALSE(ec_public_key.empty());

    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::ES256;
    create_options.key       = ec_private_key;
    std::string token        = jwt::create(payload, create_options);

    // Verify with public key
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::ES256;
    verify_options.key       = ec_public_key;
    auto result              = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");

    // Negative: a different EC key must not verify.
    auto [other_private, other_public] = crypto::generate_ec_keypair("prime256v1");
    verify_options.key                 = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
}

// Test EdDSA signatures (Ed25519). Keys generated in-process.
TEST(CryptoJWT, EdDSASignature) {
    auto [ed25519_private_key, ed25519_public_key] = crypto::generate_ed25519_keypair();
    ASSERT_FALSE(ed25519_private_key.empty());
    ASSERT_FALSE(ed25519_public_key.empty());

    std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::EdDSA;
    create_options.key       = ed25519_private_key;
    std::string token        = jwt::create(payload, create_options);

    // Verify with public key
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::EdDSA;
    verify_options.key       = ed25519_public_key;
    auto result              = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    ASSERT_EQ(result.payload.at("user_id"), "12345");

    // Negative: a different Ed25519 key must not verify.
    auto [other_private, other_public] = crypto::generate_ed25519_keypair();
    verify_options.key                 = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
}

// Test custom claim validation
TEST(CryptoJWT, CustomClaimValidation) {
    // Create a token with custom claims
    std::map<std::string, std::string> payload = {{"user_id", "12345"}, {"role", "admin"}, {"organization", "test-org"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    std::string token = jwt::create(payload, create_options);

    // Verify with required custom claims
    jwt::VerifyOptions verify_options;
    verify_options.algorithm       = jwt::Algorithm::HS256;
    verify_options.key             = "your-secret-key";
    verify_options.required_claims = {{"role", "admin"}, {"organization", "test-org"}};

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

TEST(CryptoJWT, AlgorithmMappingCoversAllPublicValues) {
    const std::vector<std::pair<jwt::Algorithm, std::string>> algorithms = {
        {jwt::Algorithm::HS256, "HS256"}, {jwt::Algorithm::HS384, "HS384"}, {jwt::Algorithm::HS512, "HS512"}, {jwt::Algorithm::RS256, "RS256"},
        {jwt::Algorithm::RS384, "RS384"}, {jwt::Algorithm::RS512, "RS512"}, {jwt::Algorithm::ES256, "ES256"}, {jwt::Algorithm::ES384, "ES384"},
        {jwt::Algorithm::ES512, "ES512"}, {jwt::Algorithm::EdDSA, "EdDSA"},
    };

    for (const auto &[algorithm, name] : algorithms) {
        EXPECT_EQ(jwt::algorithm_to_string(algorithm), name);
        ASSERT_TRUE(jwt::algorithm_from_string(name).has_value());
        EXPECT_EQ(jwt::algorithm_from_string(name).value(), algorithm);
    }

    EXPECT_EQ(jwt::algorithm_to_string(static_cast<jwt::Algorithm>(255)), "unknown");
    EXPECT_FALSE(jwt::algorithm_from_string("none").has_value());
}

TEST(CryptoJWT, HmacSha384AndSha512Tokens) {
    const std::map<std::string, std::string> payload = {{"scope", "extended"}};

    for (const auto algorithm : {jwt::Algorithm::HS384, jwt::Algorithm::HS512}) {
        jwt::CreateOptions create_options;
        create_options.algorithm     = algorithm;
        create_options.key           = "strong-shared-secret";
        create_options.type          = "JWT";
        create_options.content_type  = "application/json";
        create_options.key_id        = "key-1";
        create_options.header_claims = {{"tenant", "qb"}};

        const std::string token   = jwt::create(payload, create_options);
        const auto        decoded = jwt::decode(token);
        EXPECT_NE(decoded.header.find("\"cty\":\"application/json\""), std::string::npos);
        EXPECT_NE(decoded.header.find("\"kid\":\"key-1\""), std::string::npos);
        EXPECT_NE(decoded.header.find("\"tenant\":\"qb\""), std::string::npos);

        jwt::VerifyOptions verify_options;
        verify_options.algorithm = algorithm;
        verify_options.key       = "strong-shared-secret";

        const auto result = jwt::verify(token, verify_options);
        ASSERT_TRUE(result.is_valid());
        EXPECT_EQ(result.payload.at("scope"), "extended");
    }
}

TEST(CryptoJWT, RsaAndEcdsaSha384Sha512Tokens) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    auto [rsa_private_key, rsa_public_key] = crypto::generate_rsa_keypair(2048);
    for (const auto algorithm : {jwt::Algorithm::RS384, jwt::Algorithm::RS512}) {
        jwt::CreateOptions create_options;
        create_options.algorithm = algorithm;
        create_options.key       = rsa_private_key;
        const std::string token  = jwt::create(payload, create_options);

        jwt::VerifyOptions verify_options;
        verify_options.algorithm = algorithm;
        verify_options.key       = rsa_public_key;
        EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());
    }

    const std::vector<std::tuple<jwt::Algorithm, std::string>> ec_cases = {
        {jwt::Algorithm::ES384, "secp384r1"},
        {jwt::Algorithm::ES512, "secp521r1"},
    };
    for (const auto &[algorithm, curve] : ec_cases) {
        auto [ec_private_key, ec_public_key] = crypto::generate_ec_keypair(curve);

        jwt::CreateOptions create_options;
        create_options.algorithm = algorithm;
        create_options.key       = ec_private_key;
        const std::string token  = jwt::create(payload, create_options);

        jwt::VerifyOptions verify_options;
        verify_options.algorithm = algorithm;
        verify_options.key       = ec_public_key;
        EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());
    }
}

TEST(CryptoJWT, DecodeAndVerifyRejectMalformedTokens) {
    EXPECT_THROW(jwt::decode("one.two"), std::runtime_error);
    EXPECT_THROW(jwt::decode("a.b.c"), std::runtime_error);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "secret";

    EXPECT_EQ(jwt::verify("one.two", verify_options).error, jwt::ValidationError::INVALID_FORMAT);
    EXPECT_EQ(jwt::verify("a.b.c", verify_options).error, jwt::ValidationError::INVALID_FORMAT);

    const std::string missing_alg = to_base64url("{}") + "." + to_base64url("{}") + ".signature";
    EXPECT_EQ(jwt::verify(missing_alg, verify_options).error, jwt::ValidationError::INVALID_FORMAT);

    const std::string wrong_alg = to_base64url("{\"alg\":\"HS512\"}") + "." + to_base64url("{}") + ".signature";
    EXPECT_EQ(jwt::verify(wrong_alg, verify_options).error, jwt::ValidationError::INVALID_SIGNATURE);

    const std::string invalid_payload = to_base64url("{\"alg\":\"HS256\"}") + ".a.signature";
    EXPECT_EQ(jwt::verify(invalid_payload, verify_options).error, jwt::ValidationError::INVALID_FORMAT);
}

TEST(CryptoJWT, StandardClaimMismatchErrors) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "secret";
    const std::string token =
        jwt::create_token(payload, "issuer", "subject", "audience", std::chrono::hours(1), std::chrono::seconds(0), "jwt-id", create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "secret";

    verify_options.verify_issuer = true;
    verify_options.issuer        = "wrong";
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::INVALID_ISSUER);

    verify_options.verify_issuer   = false;
    verify_options.verify_audience = true;
    verify_options.audience        = "wrong";
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::INVALID_AUDIENCE);

    verify_options.verify_audience = false;
    verify_options.verify_subject  = true;
    verify_options.subject         = "wrong";
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::INVALID_SUBJECT);

    verify_options.verify_subject = false;
    verify_options.verify_jti     = true;
    verify_options.jti            = "wrong";
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::CLAIM_MISMATCH);
}

TEST(CryptoJWT, NumericDateClaimsRejectMalformedValues) {
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "secret";

    const std::string  bad_exp = jwt::create({{"exp", "not-a-number"}}, create_options);
    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "secret";
    verify_options.verify_expiration = true;
    EXPECT_EQ(jwt::verify(bad_exp, verify_options).error, jwt::ValidationError::INVALID_FORMAT);

    const std::string bad_nbf        = jwt::create({{"nbf", "123x"}}, create_options);
    verify_options.verify_expiration = false;
    verify_options.verify_not_before = true;
    EXPECT_EQ(jwt::verify(bad_nbf, verify_options).error, jwt::ValidationError::INVALID_FORMAT);
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
