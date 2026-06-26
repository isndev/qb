/**
 * @file unit/crypto/crypto-jwt.cpp
 * @brief `qb::jwt` create / verify / decode — pure logic, no engine.
 *
 * The JWT surface in qb/io/crypto_jwt.h is a deterministic, in-process implementation of
 * RFC 7519: HMAC / RSA / ECDSA / EdDSA signing, standard-claim validation, and the
 * `ValidationError` taxonomy. It links libcrypto but spins no event loop, opens no socket
 * and needs no daemon, so it is a `unit` test. The contracts proven:
 *
 *   - HS256/384/512 round-trip with full claim read-back and surviving header claims;
 *   - RS256/384/512, ES256/384/512 and EdDSA sign with a generated private key and verify
 *     with the matching public key, and a different key fails (negative control);
 *   - standard-claim validation (iss/sub/aud/jti) and required custom-claim matching, each
 *     mismatch mapping to its specific ValidationError;
 *   - expiry and not-before are enforced — proven with a relative clock (a token stamped in
 *     the past / future), NOT a wall-clock sleep — and clock-skew widens the window;
 *   - the algorithm enum<->string mapping covers every public value plus the sentinels;
 *   - malformed/segment-count/non-numeric-date tokens map to INVALID_FORMAT, a wrong
 *     declared algorithm maps to INVALID_SIGNATURE;
 *   - the `alg:"none"` downgrade attack is rejected (a token claiming `none` never verifies
 *     under a configured HMAC verifier — the canonical JWT vulnerability).
 *
 * Restructured from the dissolved system/test-crypto-jwt.cpp: the two real-time
 * `std::this_thread::sleep_for` waits in `Expiration` (2 s) and `NotBeforeValidation` (3 s)
 * are replaced by tokens whose `exp`/`nbf` claims are stamped relative to the verifier's own
 * epoch-seconds clock (deterministic, zero wall-clock wait); the dead `read_file()` helper is
 * deleted; an `alg:"none"` rejection test is added; the file-local main() is dropped for the
 * shared gtest_main.
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
 * @ingroup Tests
 */

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/crypto.h>
#include <qb/io/crypto_jwt.h>

using namespace qb;

namespace {

/**
 * @brief Current Unix time in seconds — mirrors jwt::current_timestamp() so a token's
 *        exp/nbf claim can be stamped relative to "now" without ever sleeping.
 */
int64_t
unix_now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Encode a raw string as base64url with padding stripped (JWT segment encoding).
 */
std::string
to_base64url(const std::string &data) {
    std::string encoded = crypto::base64::encode(data);
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    if (const auto padding = encoded.find('='); padding != std::string::npos) {
        encoded.erase(padding);
    }
    return encoded;
}

} // namespace

// =============================================================================
// HMAC TOKENS
// =============================================================================

TEST(CryptoJWT, BasicHmacToken) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}, {"username", "testuser"}, {"role", "admin"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    const std::string token = jwt::create(payload, create_options);

    // Three dot-separated parts.
    ASSERT_NE(token.find_first_of('.'), std::string::npos);
    ASSERT_NE(token.find_last_of('.'), token.find_first_of('.'));

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "your-secret-key";

    const auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.payload.at("user_id"), "12345");
    EXPECT_EQ(result.payload.at("username"), "testuser");
    EXPECT_EQ(result.payload.at("role"), "admin");
}

TEST(CryptoJWT, StandardClaims) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    const std::string token = jwt::create_token(payload, "test-issuer", "user-12345", "test-audience", std::chrono::hours(1),
                                                std::chrono::seconds(0), "token-id-123", create_options);

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

    const auto result = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.payload.at("user_id"), "12345");
    EXPECT_EQ(result.payload.at("iss"), "test-issuer");
    EXPECT_EQ(result.payload.at("sub"), "user-12345");
    EXPECT_EQ(result.payload.at("aud"), "test-audience");
    EXPECT_EQ(result.payload.at("jti"), "token-id-123");
    EXPECT_TRUE(result.payload.contains("iat"));
    EXPECT_TRUE(result.payload.contains("exp"));
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

// =============================================================================
// EXPIRY / NOT-BEFORE — relative clock (no sleep)
// =============================================================================

TEST(CryptoJWT, ExpirationIsEnforcedWithoutSleeping) {
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    // A token whose exp is 100 s in the past — already expired the moment it exists.
    // create_token stringifies exp, so a string claim is the on-the-wire representation.
    const std::string expired_token = jwt::create({{"user_id", "12345"}, {"exp", std::to_string(unix_now_seconds() - 100)}}, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "your-secret-key";
    verify_options.verify_expiration = true;

    const auto expired = jwt::verify(expired_token, verify_options);
    EXPECT_FALSE(expired.is_valid());
    EXPECT_EQ(expired.error, jwt::ValidationError::TOKEN_EXPIRED);

    // A generous clock skew brings the same token back inside the window.
    verify_options.clock_skew = std::chrono::seconds(200);
    EXPECT_TRUE(jwt::verify(expired_token, verify_options).is_valid());

    // A token whose exp is well in the future verifies cleanly.
    verify_options.clock_skew       = std::chrono::seconds(0);
    const std::string live_token    = jwt::create({{"user_id", "12345"}, {"exp", std::to_string(unix_now_seconds() + 3600)}}, create_options);
    EXPECT_TRUE(jwt::verify(live_token, verify_options).is_valid());
}

TEST(CryptoJWT, NotBeforeIsEnforcedWithoutSleeping) {
    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";

    // nbf is 1 hour in the future — the token is not yet active.
    const std::string future_token = jwt::create({{"user_id", "12345"}, {"nbf", std::to_string(unix_now_seconds() + 3600)}}, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "your-secret-key";
    verify_options.verify_not_before = true;

    const auto not_active = jwt::verify(future_token, verify_options);
    EXPECT_FALSE(not_active.is_valid());
    EXPECT_EQ(not_active.error, jwt::ValidationError::TOKEN_NOT_ACTIVE);

    // A token whose nbf is in the past is active now.
    const std::string active_token = jwt::create({{"user_id", "12345"}, {"nbf", std::to_string(unix_now_seconds() - 100)}}, create_options);
    EXPECT_TRUE(jwt::verify(active_token, verify_options).is_valid());
}

// =============================================================================
// SIGNATURE VALIDATION
// =============================================================================

TEST(CryptoJWT, SignatureValidation) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";
    const std::string token  = jwt::create(payload, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "your-secret-key";
    EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());

    verify_options.key  = "wrong-secret-key";
    const auto rejected = jwt::verify(token, verify_options);
    EXPECT_FALSE(rejected.is_valid());
    EXPECT_EQ(rejected.error, jwt::ValidationError::INVALID_SIGNATURE);
}

TEST(CryptoJWT, RejectsAlgNoneDowngradeAttack) {
    // The classic JWT vulnerability: an attacker rewrites the header to `alg:"none"`
    // and strips the signature, hoping the verifier accepts an unsigned token. A
    // verifier configured for HS256 must reject it. algorithm_from_string("none")
    // yields no enumerator, so verify() returns INVALID_SIGNATURE (never NONE).
    const std::string header_none  = to_base64url("{\"alg\":\"none\",\"typ\":\"JWT\"}");
    const std::string payload_part = to_base64url("{\"user_id\":\"12345\",\"role\":\"admin\"}");

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "your-secret-key";

    // Empty signature (the canonical attack shape).
    EXPECT_NE(jwt::verify(header_none + "." + payload_part + ".", verify_options).error, jwt::ValidationError::NONE);
    // Non-empty but meaningless signature.
    EXPECT_NE(jwt::verify(header_none + "." + payload_part + ".signature", verify_options).error, jwt::ValidationError::NONE);
}

// =============================================================================
// ASYMMETRIC SIGNATURES — keys generated in-process
// =============================================================================

TEST(CryptoJWT, RSASignature) {
    auto [private_key, public_key] = crypto::generate_rsa_keypair(2048);
    ASSERT_FALSE(private_key.empty());
    ASSERT_FALSE(public_key.empty());

    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::RS256;
    create_options.key       = private_key;
    const std::string token  = jwt::create(payload, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::RS256;
    verify_options.key       = public_key;
    const auto result        = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.payload.at("user_id"), "12345");

    // A token signed by a different key must NOT verify.
    auto [_, other_public] = crypto::generate_rsa_keypair(2048);
    verify_options.key     = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
}

TEST(CryptoJWT, ECDSASignature) {
    auto [private_key, public_key] = crypto::generate_ec_keypair("prime256v1");
    ASSERT_FALSE(private_key.empty());
    ASSERT_FALSE(public_key.empty());

    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::ES256;
    create_options.key       = private_key;
    const std::string token  = jwt::create(payload, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::ES256;
    verify_options.key       = public_key;
    const auto result        = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.payload.at("user_id"), "12345");

    auto [_, other_public] = crypto::generate_ec_keypair("prime256v1");
    verify_options.key     = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
}

TEST(CryptoJWT, EdDSASignature) {
    auto [private_key, public_key] = crypto::generate_ed25519_keypair();
    ASSERT_FALSE(private_key.empty());
    ASSERT_FALSE(public_key.empty());

    const std::map<std::string, std::string> payload = {{"user_id", "12345"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::EdDSA;
    create_options.key       = private_key;
    const std::string token  = jwt::create(payload, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::EdDSA;
    verify_options.key       = public_key;
    const auto result        = jwt::verify(token, verify_options);
    ASSERT_TRUE(result.is_valid());
    EXPECT_EQ(result.payload.at("user_id"), "12345");

    auto [_, other_public] = crypto::generate_ed25519_keypair();
    verify_options.key     = other_public;
    EXPECT_FALSE(jwt::verify(token, verify_options).is_valid());
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

// =============================================================================
// CLAIM VALIDATION + ERROR TAXONOMY
// =============================================================================

TEST(CryptoJWT, CustomClaimValidation) {
    const std::map<std::string, std::string> payload = {{"user_id", "12345"}, {"role", "admin"}, {"organization", "test-org"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "your-secret-key";
    const std::string token  = jwt::create(payload, create_options);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm       = jwt::Algorithm::HS256;
    verify_options.key             = "your-secret-key";
    verify_options.required_claims = {{"role", "admin"}, {"organization", "test-org"}};
    EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());

    // Wrong required-claim value.
    verify_options.required_claims = {{"role", "user"}};
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::CLAIM_MISMATCH);

    // Missing required claim.
    verify_options.required_claims = {{"department", "engineering"}};
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::CLAIM_MISMATCH);
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

// =============================================================================
// WAVE-3 ADDITIONS — uncovered reachable branches in crypto_jwt.cpp
//
// These cases target gaps the existing suite leaves open:
//   - create_token's `nbf` branch (not_before > 0)                       [src 298-300]
//   - decode()'s payload-base64 catch (invalid segment[1])               [src 355-357]
//   - verify()'s INVALID_FORMAT on bad payload base64 / non-JSON payload [src 421-431]
//   - verify()'s INVALID_FORMAT on an undecodable signature segment      [src 440-442]
//   - read_numeric_date's NUMBER variants (integer / unsigned / float)   [src 464-475]
//   - read_numeric_date rejecting a non-numeric, non-string exp          [src 486]
//   - the `aud` JSON-array audience match (RFC 7519)                     [src 523-530]
//
// To exercise the numeric-typed and array-typed claim branches we hand-build a
// token whose payload JSON carries the real type (jwt::create stringifies every
// value, which only ever hits the is_string branch). The token is signed with a
// genuine HS256 MAC so verification reaches the claim-validation stage rather
// than bailing at INVALID_SIGNATURE.
// =============================================================================

namespace {

/**
 * @brief Build a correctly-HS256-signed JWT from raw header/payload JSON text.
 *
 * Mirrors jwt::create's wire format (base64url header.payload.signature) but
 * lets the caller control the exact JSON so a claim can be a JSON number, bool,
 * array, etc. — types jwt::create cannot emit.
 */
std::string
make_hs256_token(const std::string &payload_json, const std::string &secret, const std::string &header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}") {
    const std::string header_b64  = to_base64url(header_json);
    const std::string payload_b64 = to_base64url(payload_json);
    const std::string signing_in  = header_b64 + "." + payload_b64;

    const std::vector<unsigned char> data(signing_in.begin(), signing_in.end());
    const std::vector<unsigned char> key(secret.begin(), secret.end());
    const auto                       mac = crypto::hmac(data, key, crypto::DigestAlgorithm::SHA256);

    // The JWT signature segment is base64url(raw_mac_bytes). to_base64url already
    // base64-encodes its input, so feed it the raw MAC bytes directly. Pre-running
    // crypto::base64::encode here would add a SECOND base64 layer, and the verifier
    // (crypto_jwt.cpp: base64url_to_base64 + base64::decode, once) would then recover
    // the inner base64 *text* instead of the 32 raw HMAC bytes -> INVALID_SIGNATURE.
    const std::string sig_b64 = to_base64url(std::string(mac.begin(), mac.end()));
    return signing_in + "." + sig_b64;
}

} // namespace

TEST(CryptoJWT, CreateTokenStampsNotBeforeWhenPositive) {
    // not_before > 0 takes the `nbf` branch in create_token (src 298-300); the
    // produced nbf must sit `not_before` seconds in the future and the token must
    // therefore be rejected as TOKEN_NOT_ACTIVE by a not-before-checking verifier.
    const std::map<std::string, std::string> payload = {{"user_id", "42"}};

    jwt::CreateOptions create_options;
    create_options.algorithm = jwt::Algorithm::HS256;
    create_options.key       = "secret";

    const int64_t     before = unix_now_seconds();
    const std::string token  = jwt::create_token(payload, /*issuer*/ "", /*subject*/ "", /*audience*/ "", /*expires_in*/ std::chrono::seconds(0),
                                                /*not_before*/ std::chrono::hours(1), /*jti*/ "", create_options);

    const auto decoded = jwt::decode(token);
    // nbf is present and ~1h ahead of the stamp instant.
    const auto nbf_pos = decoded.payload.find("\"nbf\":\"");
    ASSERT_NE(nbf_pos, std::string::npos);
    const int64_t nbf = std::stoll(decoded.payload.substr(nbf_pos + 7));
    EXPECT_GE(nbf, before + 3600);
    EXPECT_LE(nbf, before + 3600 + 5);

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = "secret";
    verify_options.verify_not_before = true;
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::TOKEN_NOT_ACTIVE);
}

TEST(CryptoJWT, DecodeRejectsUndecodablePayloadSegment) {
    // A header that decodes fine, a payload segment that is NOT valid base64url:
    // a single stray char makes length % 4 == 1, which base64url_to_base64 throws
    // on — decode() maps that to "Invalid JWT payload encoding" (src 355-357).
    const std::string good_header = to_base64url("{\"alg\":\"HS256\"}");
    EXPECT_THROW(jwt::decode(good_header + ".x.signature"), std::runtime_error);
}

TEST(CryptoJWT, VerifyRejectsUndecodableAndNonJsonPayload) {
    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "secret";

    const std::string good_header = to_base64url("{\"alg\":\"HS256\"}");

    // Payload segment that fails base64url decoding (length % 4 == 1) -> the
    // base64url_to_base64 throw is caught and mapped to INVALID_FORMAT (src 421-423).
    EXPECT_EQ(jwt::verify(good_header + ".x.signature", verify_options).error, jwt::ValidationError::INVALID_FORMAT);

    // Payload that decodes from base64 but is not valid JSON -> json::parse throws,
    // mapped to INVALID_FORMAT (src 426-431).
    const std::string not_json_payload = to_base64url("this is not json {");
    EXPECT_EQ(jwt::verify(good_header + "." + not_json_payload + ".signature", verify_options).error, jwt::ValidationError::INVALID_FORMAT);
}

TEST(CryptoJWT, VerifyRejectsUndecodableSignatureSegment) {
    // Header (HS256) and payload both decode and parse cleanly, but the signature
    // segment is not valid base64url (length % 4 == 1): the base64url_to_base64
    // throw on the signature is caught and mapped to INVALID_FORMAT (src 440-442),
    // a distinct path from the INVALID_SIGNATURE returned for a well-formed-but-wrong MAC.
    const std::string header  = to_base64url("{\"alg\":\"HS256\"}");
    const std::string payload = to_base64url("{\"user_id\":\"1\"}");

    jwt::VerifyOptions verify_options;
    verify_options.algorithm = jwt::Algorithm::HS256;
    verify_options.key       = "secret";

    EXPECT_EQ(jwt::verify(header + "." + payload + ".x", verify_options).error, jwt::ValidationError::INVALID_FORMAT);
}

TEST(CryptoJWT, NumericTypedExpClaimIsAcceptedAndEnforced) {
    // RFC 7519 defines exp as a NumericDate (a JSON number). A standards-compliant
    // external token carries exp as a raw number, not a string. read_numeric_date's
    // integer branch (src 464-466) must accept it; the float branch (src 472-475)
    // and unsigned branch (src 468-471) are exercised by the other claims below.
    const std::string secret = "secret";

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = secret;
    verify_options.verify_expiration = true;

    // exp as a plain (signed) integer in the past -> integer branch, then expired.
    const std::string expired = make_hs256_token("{\"exp\":" + std::to_string(unix_now_seconds() - 100) + "}", secret);
    EXPECT_EQ(jwt::verify(expired, verify_options).error, jwt::ValidationError::TOKEN_EXPIRED);

    // exp as a plain integer well in the future -> integer branch, valid.
    const std::string live = make_hs256_token("{\"exp\":" + std::to_string(unix_now_seconds() + 3600) + "}", secret);
    EXPECT_TRUE(jwt::verify(live, verify_options).is_valid());

    // exp as a JSON float (fractional NumericDate) in the future -> float branch (src 472-475).
    const std::string live_float = make_hs256_token("{\"exp\":" + std::to_string(unix_now_seconds() + 3600) + ".5}", secret);
    EXPECT_TRUE(jwt::verify(live_float, verify_options).is_valid());

    // exp as a JSON float in the past -> float branch, expired.
    const std::string expired_float = make_hs256_token("{\"exp\":" + std::to_string(unix_now_seconds() - 100) + ".25}", secret);
    EXPECT_EQ(jwt::verify(expired_float, verify_options).error, jwt::ValidationError::TOKEN_EXPIRED);
}

TEST(CryptoJWT, NumericTypedNbfIntegerAndFloatBranches) {
    const std::string secret = "secret";

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = secret;
    verify_options.verify_not_before = true;

    // nbf as a raw JSON integer in the future -> read_numeric_date's integer
    // branch (src 464-466; nlohmann's is_number_integer() is true for unsigned
    // too and is checked first, so the is_number_unsigned-only branch at src
    // 468-471 is unreachable through any JSON literal). The token is not yet active.
    const std::string future_int = make_hs256_token("{\"nbf\":" + std::to_string(unix_now_seconds() + 3600) + "}", secret);
    EXPECT_EQ(jwt::verify(future_int, verify_options).error, jwt::ValidationError::TOKEN_NOT_ACTIVE);

    // nbf as a JSON float in the past -> float branch (src 472-475), active now.
    const std::string past_float = make_hs256_token("{\"nbf\":" + std::to_string(unix_now_seconds() - 100) + ".0}", secret);
    EXPECT_TRUE(jwt::verify(past_float, verify_options).is_valid());
}

TEST(CryptoJWT, NonNumericExpClaimIsRejectedAsInvalidFormat) {
    // exp present but neither a number nor a numeric string -> read_numeric_date
    // falls through every branch and returns false (src 486), and verify() maps
    // that to INVALID_FORMAT (fail-closed on a malformed NumericDate).
    const std::string secret = "secret";

    jwt::VerifyOptions verify_options;
    verify_options.algorithm         = jwt::Algorithm::HS256;
    verify_options.key               = secret;
    verify_options.verify_expiration = true;

    // exp as a JSON boolean.
    const std::string bool_exp = make_hs256_token("{\"exp\":true}", secret);
    EXPECT_EQ(jwt::verify(bool_exp, verify_options).error, jwt::ValidationError::INVALID_FORMAT);

    // exp as a JSON array (also non-numeric/non-string).
    const std::string array_exp = make_hs256_token("{\"exp\":[1,2,3]}", secret);
    EXPECT_EQ(jwt::verify(array_exp, verify_options).error, jwt::ValidationError::INVALID_FORMAT);
}

TEST(CryptoJWT, AudienceMatchesAgainstJsonArrayClaim) {
    // RFC 7519: `aud` may be an array of strings; verify() must match if ANY
    // element equals the expected audience (src 523-530). jwt::create can only
    // emit a string aud, so we hand-build the array form.
    const std::string secret = "secret";

    jwt::VerifyOptions verify_options;
    verify_options.algorithm       = jwt::Algorithm::HS256;
    verify_options.key             = secret;
    verify_options.verify_audience = true;

    const std::string token = make_hs256_token("{\"aud\":[\"svc-a\",\"svc-b\",\"svc-c\"]}", secret);

    // An audience present in the array passes.
    verify_options.audience = "svc-b";
    EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());

    // The last element is matched too (loop must scan to the end).
    verify_options.audience = "svc-c";
    EXPECT_TRUE(jwt::verify(token, verify_options).is_valid());

    // An audience absent from the array fails INVALID_AUDIENCE (loop completes
    // without a hit -> aud_ok stays false).
    verify_options.audience = "svc-z";
    EXPECT_EQ(jwt::verify(token, verify_options).error, jwt::ValidationError::INVALID_AUDIENCE);
}
