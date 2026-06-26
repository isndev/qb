/**
 * @file qb/io/tests/benchmark/crypto/crypto-extras.cpp
 * @brief Benchmarks for qb crypto KDFs, asymmetric signatures, and JWT.
 *
 * Complements crypto-primitives.cpp (base64 / digest / HMAC / AEAD / PBKDF2 /
 * X25519 / ECIES) with the heavier asymmetric and KDF surface used by auth and
 * token flows:
 *   - HKDF (RFC 5869) and Argon2id key derivation;
 *   - Ed25519, RSA (esp. RS256 verify), and EC sign/verify;
 *   - JWT HS256 / RS256 create + verify round-trips.
 *
 * All keys/material are generated ONCE before the timed loop (the per-key cost
 * is not part of the measured op); ops/sec is the per-call throughput. Crypto is
 * optional — the integrator gates this target on QB_HAS_SSL.
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

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <qb/io/crypto.h>
#include <qb/io/crypto_jwt.h>

namespace {

std::vector<unsigned char>
make_bytes(std::size_t size) {
    std::vector<unsigned char> out(size);
    std::uint32_t              x = 0x9e3779b9u;
    for (auto &byte : out) {
        x    = x * 1664525u + 1013904223u;
        byte = static_cast<unsigned char>((x >> 24u) & 0xffu);
    }
    return out;
}

// A small payload to sign — representative of a token / message digest input.
const std::vector<unsigned char> &
sign_payload() {
    static const std::vector<unsigned char> p = make_bytes(256);
    return p;
}

// ---------------------------------------------------------------------------
// HKDF (RFC 5869): extract+expand from input key material to `output_length`.
// ---------------------------------------------------------------------------
void
BM_Crypto_Hkdf(benchmark::State &state) {
    const auto output_length = static_cast<std::size_t>(state.range(0));
    const auto ikm           = make_bytes(32);
    const auto salt          = make_bytes(16);
    const auto info          = make_bytes(8);

    for (auto _ : state) {
        auto key = qb::crypto::hkdf(ikm, salt, info, output_length, qb::crypto::DigestAlgorithm::SHA256);
        benchmark::DoNotOptimize(key.data());
        benchmark::DoNotOptimize(key.size());
    }

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// Argon2id: memory-hard password KDF. Parameterized on time cost (t) and memory
// cost (m, KiB) so the curve shows the work-factor trade-off.
// ---------------------------------------------------------------------------
void
BM_Crypto_Argon2id(benchmark::State &state) {
    const auto t_cost = static_cast<std::uint32_t>(state.range(0));
    const auto m_cost = static_cast<std::uint32_t>(state.range(1));

    qb::crypto::Argon2Params params;
    params.t_cost      = t_cost;
    params.m_cost      = m_cost;
    params.parallelism = 1;
    params.salt        = "qb-benchmark-salt";

    for (auto _ : state) {
        auto key = qb::crypto::argon2_kdf("qb-password", 32, params, qb::crypto::Argon2Variant::Argon2id);
        benchmark::DoNotOptimize(key.data());
        benchmark::DoNotOptimize(key.size());
    }

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// Ed25519 sign / verify (EdDSA). Keys generated once; the signature is
// pre-computed for the verify bench.
// ---------------------------------------------------------------------------
void
BM_Crypto_Ed25519Sign(benchmark::State &state) {
    const auto keys = qb::crypto::generate_ed25519_keypair(); // {private, public} PEM

    for (auto _ : state) {
        auto sig = qb::crypto::ed25519_sign(sign_payload(), keys.first);
        benchmark::DoNotOptimize(sig.data());
        benchmark::DoNotOptimize(sig.size());
    }

    state.SetItemsProcessed(state.iterations());
}

void
BM_Crypto_Ed25519Verify(benchmark::State &state) {
    const auto keys = qb::crypto::generate_ed25519_keypair();
    const auto sig  = qb::crypto::ed25519_sign(sign_payload(), keys.first);

    bool ok = false;
    for (auto _ : state) {
        ok = qb::crypto::ed25519_verify(sign_payload(), sig, keys.second);
        benchmark::DoNotOptimize(ok);
    }

    if (!ok)
        state.SkipWithError("Ed25519 verification failed — signature/key mismatch in the bench");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// RSA sign / verify (RS256 = RSASSA-PKCS1-v1_5 + SHA-256). Verify is the hot
// auth path (cheap public-key op vs. expensive private-key sign).
// ---------------------------------------------------------------------------
void
BM_Crypto_RsaSign(benchmark::State &state) {
    const auto keys = qb::crypto::generate_rsa_keypair(2048); // {private, public} PEM

    for (auto _ : state) {
        auto sig = qb::crypto::rsa_sign(sign_payload(), keys.first, qb::crypto::DigestAlgorithm::SHA256);
        benchmark::DoNotOptimize(sig.data());
        benchmark::DoNotOptimize(sig.size());
    }

    state.SetItemsProcessed(state.iterations());
}

void
BM_Crypto_RsaVerify(benchmark::State &state) {
    const auto keys = qb::crypto::generate_rsa_keypair(2048);
    const auto sig  = qb::crypto::rsa_sign(sign_payload(), keys.first, qb::crypto::DigestAlgorithm::SHA256);

    bool ok = false;
    for (auto _ : state) {
        ok = qb::crypto::rsa_verify(sign_payload(), sig, keys.second, qb::crypto::DigestAlgorithm::SHA256);
        benchmark::DoNotOptimize(ok);
    }

    if (!ok)
        state.SkipWithError("RSA (RS256) verification failed — signature/key mismatch in the bench");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// EC (ECDSA, prime256v1 + SHA-256) sign / verify.
// ---------------------------------------------------------------------------
void
BM_Crypto_EcSign(benchmark::State &state) {
    const auto keys = qb::crypto::generate_ec_keypair("prime256v1");

    for (auto _ : state) {
        auto sig = qb::crypto::ec_sign(sign_payload(), keys.first, qb::crypto::DigestAlgorithm::SHA256);
        benchmark::DoNotOptimize(sig.data());
        benchmark::DoNotOptimize(sig.size());
    }

    state.SetItemsProcessed(state.iterations());
}

void
BM_Crypto_EcVerify(benchmark::State &state) {
    const auto keys = qb::crypto::generate_ec_keypair("prime256v1");
    const auto sig  = qb::crypto::ec_sign(sign_payload(), keys.first, qb::crypto::DigestAlgorithm::SHA256);

    bool ok = false;
    for (auto _ : state) {
        ok = qb::crypto::ec_verify(sign_payload(), sig, keys.second, qb::crypto::DigestAlgorithm::SHA256);
        benchmark::DoNotOptimize(ok);
    }

    if (!ok)
        state.SkipWithError("EC verification failed — signature/key mismatch in the bench");

    state.SetItemsProcessed(state.iterations());
}

// A representative token payload (claim map) reused by every JWT bench.
std::map<std::string, std::string>
jwt_payload() {
    return {{"role", "admin"}, {"scope", "read:write"}, {"tenant", "qb"}};
}

// ---------------------------------------------------------------------------
// JWT HS256 create + verify round-trip. HMAC-signed — the symmetric-key path.
// ---------------------------------------------------------------------------
void
BM_Jwt_Hs256CreateVerify(benchmark::State &state) {
    qb::jwt::CreateOptions create_opts;
    create_opts.algorithm = qb::jwt::Algorithm::HS256;
    create_opts.key       = "qb-shared-secret-key-for-hs256-benchmark";

    qb::jwt::VerifyOptions verify_opts;
    verify_opts.algorithm         = qb::jwt::Algorithm::HS256;
    verify_opts.key               = create_opts.key;
    verify_opts.verify_expiration = false; // no exp claim in this payload

    const auto payload = jwt_payload();

    bool ok = false;
    for (auto _ : state) {
        auto token  = qb::jwt::create(payload, create_opts);
        auto result = qb::jwt::verify(token, verify_opts);
        ok          = result.is_valid();
        benchmark::DoNotOptimize(token.data());
        benchmark::DoNotOptimize(ok);
    }

    if (!ok)
        state.SkipWithError("JWT HS256 create/verify round-trip failed");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// JWT RS256 create + verify round-trip. RSA-signed — the asymmetric token path
// (verify dominated by the public-key op, create by the private-key sign).
// ---------------------------------------------------------------------------
void
BM_Jwt_Rs256CreateVerify(benchmark::State &state) {
    const auto rsa_keys = qb::crypto::generate_rsa_keypair(2048);

    qb::jwt::CreateOptions create_opts;
    create_opts.algorithm = qb::jwt::Algorithm::RS256;
    create_opts.key       = rsa_keys.first; // private key PEM

    qb::jwt::VerifyOptions verify_opts;
    verify_opts.algorithm         = qb::jwt::Algorithm::RS256;
    verify_opts.key               = rsa_keys.second; // public key PEM
    verify_opts.verify_expiration = false;

    const auto payload = jwt_payload();

    bool ok = false;
    for (auto _ : state) {
        auto token  = qb::jwt::create(payload, create_opts);
        auto result = qb::jwt::verify(token, verify_opts);
        ok          = result.is_valid();
        benchmark::DoNotOptimize(token.data());
        benchmark::DoNotOptimize(ok);
    }

    if (!ok)
        state.SkipWithError("JWT RS256 create/verify round-trip failed");

    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_Crypto_Hkdf)->Arg(32)->Arg(64)->Arg(256)->ArgName("out_bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_Argon2id)
    ->Args({1, 1 << 12})  // t=1, m=4 MiB
    ->Args({3, 1 << 16})  // t=3, m=64 MiB (OWASP-ish)
    ->ArgNames({"t_cost", "m_cost_kib"})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Crypto_Ed25519Sign)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_Ed25519Verify)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_RsaSign)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_RsaVerify)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_EcSign)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_EcVerify)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Jwt_Hs256CreateVerify)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Jwt_Rs256CreateVerify)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
