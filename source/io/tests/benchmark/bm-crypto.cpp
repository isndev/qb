/**
 * @file qb/io/tests/benchmark/bm-crypto.cpp
 * @brief Benchmarks for qb crypto helpers backed by OpenSSL.
 *
 * Crypto is optional and only built when QB_HAS_SSL is enabled. The scenarios
 * focus on primitives exposed by qb::crypto and used by higher-level IO code:
 * base64, digesting, HMAC, AEAD encryption/decryption, random bytes, and PBKDF2.
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

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <qb/io/crypto.h>

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

std::string
as_string(std::vector<unsigned char> const &data) {
    return {reinterpret_cast<const char *>(data.data()), data.size()};
}

void
BM_Crypto_Base64Encode(benchmark::State &state) {
    const auto data = make_bytes(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        auto encoded = qb::crypto::base64_encode(data.data(), data.size());
        benchmark::DoNotOptimize(encoded.data());
        benchmark::DoNotOptimize(encoded.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_Base64Decode(benchmark::State &state) {
    const auto data    = make_bytes(static_cast<std::size_t>(state.range(0)));
    const auto encoded = qb::crypto::base64_encode(data.data(), data.size());

    for (auto _ : state) {
        auto decoded = qb::crypto::base64_decode(encoded);
        benchmark::DoNotOptimize(decoded.data());
        benchmark::DoNotOptimize(decoded.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(encoded.size()));
}

void
BM_Crypto_Sha256String(benchmark::State &state) {
    const auto data = as_string(make_bytes(static_cast<std::size_t>(state.range(0))));

    for (auto _ : state) {
        auto digest = qb::crypto::sha256(data);
        benchmark::DoNotOptimize(digest.data());
        benchmark::DoNotOptimize(digest.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_Sha512Stream(benchmark::State &state) {
    const auto data = as_string(make_bytes(static_cast<std::size_t>(state.range(0))));

    for (auto _ : state) {
        std::istringstream stream(data);
        auto               digest = qb::crypto::sha512(stream);
        benchmark::DoNotOptimize(digest.data());
        benchmark::DoNotOptimize(digest.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_HmacSha256(benchmark::State &state) {
    const auto data = as_string(make_bytes(static_cast<std::size_t>(state.range(0))));
    const auto key  = make_bytes(32);

    for (auto _ : state) {
        auto mac = qb::crypto::hmac_sha256(key, data);
        benchmark::DoNotOptimize(mac.data());
        benchmark::DoNotOptimize(mac.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_AesGcmEncrypt(benchmark::State &state) {
    const auto data = make_bytes(static_cast<std::size_t>(state.range(0)));
    const auto key  = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const auto iv   = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    for (auto _ : state) {
        auto encrypted = qb::crypto::encrypt(data, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        benchmark::DoNotOptimize(encrypted.data());
        benchmark::DoNotOptimize(encrypted.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_AesGcmDecrypt(benchmark::State &state) {
    const auto data      = make_bytes(static_cast<std::size_t>(state.range(0)));
    const auto key       = qb::crypto::generate_key(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const auto iv        = qb::crypto::generate_iv(qb::crypto::SymmetricAlgorithm::AES_256_GCM);
    const auto encrypted = qb::crypto::encrypt(data, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);

    for (auto _ : state) {
        auto decrypted = qb::crypto::decrypt(encrypted, key, iv, qb::crypto::SymmetricAlgorithm::AES_256_GCM);
        benchmark::DoNotOptimize(decrypted.data());
        benchmark::DoNotOptimize(decrypted.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_RandomBytes(benchmark::State &state) {
    const auto size = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        auto bytes = qb::crypto::generate_random_bytes(size);
        benchmark::DoNotOptimize(bytes.data());
        benchmark::DoNotOptimize(bytes.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_Crypto_Pbkdf2(benchmark::State &state) {
    const auto iterations = static_cast<std::size_t>(state.range(0));
    const auto key_size   = static_cast<std::size_t>(state.range(1));

    for (auto _ : state) {
        auto key = qb::crypto::pbkdf2("qb-password", "qb-salt", iterations, key_size);
        benchmark::DoNotOptimize(key.data());
        benchmark::DoNotOptimize(key.size());
    }

    state.SetItemsProcessed(state.iterations());
}

void
BM_Crypto_X25519KeypairBytes(benchmark::State &state) {
    for (auto _ : state) {
        auto keys = qb::crypto::generate_x25519_keypair_bytes();
        benchmark::DoNotOptimize(keys.first.data());
        benchmark::DoNotOptimize(keys.second.data());
    }

    state.SetItemsProcessed(state.iterations());
}

void
BM_Crypto_EciesEncrypt(benchmark::State &state) {
    const auto data = make_bytes(static_cast<std::size_t>(state.range(0)));
    const auto keys = qb::crypto::generate_x25519_keypair_bytes();

    for (auto _ : state) {
        auto encrypted = qb::crypto::ecies_encrypt(data, keys.second, {}, qb::crypto::ECIESMode::AES_GCM);
        benchmark::DoNotOptimize(encrypted.first.data());
        benchmark::DoNotOptimize(encrypted.second.data());
        benchmark::DoNotOptimize(encrypted.second.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

void
BM_Crypto_EciesDecrypt(benchmark::State &state) {
    const auto data      = make_bytes(static_cast<std::size_t>(state.range(0)));
    const auto keys      = qb::crypto::generate_x25519_keypair_bytes();
    const auto encrypted = qb::crypto::ecies_encrypt(data, keys.second, {}, qb::crypto::ECIESMode::AES_GCM);

    for (auto _ : state) {
        auto decrypted = qb::crypto::ecies_decrypt(encrypted.second, encrypted.first, keys.first, {}, qb::crypto::ECIESMode::AES_GCM);
        benchmark::DoNotOptimize(decrypted.data());
        benchmark::DoNotOptimize(decrypted.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
}

} // namespace

BENCHMARK(BM_Crypto_Base64Encode)->Args({64})->Args({4096})->Args({256 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_Base64Decode)->Args({64})->Args({4096})->Args({256 * 1024})->ArgName("source_bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_Sha256String)->Args({64})->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_Sha512Stream)->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_HmacSha256)->Args({64})->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_AesGcmEncrypt)->Args({64})->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_AesGcmDecrypt)->Args({64})->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_RandomBytes)->Args({32})->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Crypto_Pbkdf2)->Args({1000, 32})->Args({10000, 32})->ArgNames({"iterations", "key_bytes"})->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_X25519KeypairBytes)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_EciesEncrypt)->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Crypto_EciesDecrypt)->Args({4096})->Args({1024 * 1024})->ArgName("bytes")->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
