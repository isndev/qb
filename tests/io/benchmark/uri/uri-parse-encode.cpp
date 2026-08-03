/**
 * @file qb/io/tests/benchmark/uri/uri-parse-encode.cpp
 * @brief Benchmarks for qb::io::uri parsing, encoding, and normalization.
 *
 * URI parsing sits on the hot path of connection setup, client APIs, and
 * protocol adapters. These scenarios cover absolute URLs, IPv6 authorities,
 * Unix socket URIs, query-heavy inputs, percent encoding, and path normalization.
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

#include <array>
#include <benchmark/benchmark.h>
#include <string>
#include <string_view>

#include <qb/io/uri.h>

namespace {

struct UriCase {
    std::string_view name;
    std::string_view value;
    int              af = AF_INET;
};

constexpr std::array<UriCase, 11> kUriCases{{
    {"http_basic", "http://example.com:8080/api/v1/resource?x=1&y=2#frag", AF_INET},
    {"https_queries", "https://user:pass@example.com/search?q=qb+framework&filter=a&filter=b&empty=&encoded=%7Bok%7D", AF_INET},
    {"ipv6", "https://[2001:db8:85a3::8a2e:370:7334]/metrics?window=60s&unit=ns", AF_INET6},
    {"unix", "unix:///tmp/qb-dev/socket/service.sock", AF_UNIX},
    {"relative", "assets/../images/./icons//qb.svg?mode=dark&density=2", AF_INET},
    {"tcp", "tcp://127.0.0.1:4242", AF_INET},
    // IPv6 with a scope-zone identifier (link-local %iface) — the authority
    // parser must keep the '%25eth0' literal inside the brackets.
    {"ipv6_zone", "https://[fe80::1ff:fe23:4567:890a%25eth0]:9443/health", AF_INET6},
    // Malformed / adversarial edge inputs: these drive the slow error/fallback
    // branches the happy-path cases never reach. is_valid() may be false; that is
    // the point — parsing failure must still be cheap and bounded.
    {"empty", "", AF_INET},
    {"scheme_only", "https://", AF_INET},
    {"garbage", "::::not-a-uri////???###%%%", AF_INET},
    {"unbalanced_ipv6", "http://[2001:db8::oops/path?broken=", AF_INET6},
}};

std::string
make_query_payload(std::size_t fields) {
    std::string out = "https://example.com/path?";
    for (std::size_t i = 0; i < fields; ++i) {
        if (i)
            out += '&';
        out += "field";
        out += std::to_string(i);
        out += "=value%20";
        out += std::to_string(i);
    }
    return out;
}

std::string
make_encoding_payload(std::size_t size) {
    std::string out;
    out.reserve(size);
    constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 /?:#[]@!$&'()*+,;=%{}|\\^`";
    for (std::size_t i = 0; i < size; ++i)
        out.push_back(chars[i % chars.size()]);
    return out;
}

// Payload full of INVALID percent-escapes ("%ZZ" — non-hex digits). decode()
// must detect each bad escape and fall back to preserving the literal '%', a
// different branch from the fast "%XX → byte" path BM_Uri_Decode exercises.
std::string
make_invalid_escape_payload(std::size_t size) {
    std::string out;
    out.reserve(size);
    constexpr std::string_view pattern = "%ZZtext%G1%9-%%path";
    while (out.size() < size)
        out.append(pattern.data(), std::min(pattern.size(), size - out.size()));
    return out;
}

void
BM_Uri_ParseCommon(benchmark::State &state) {
    const auto &c = kUriCases[static_cast<std::size_t>(state.range(0))];

    for (auto _ : state) {
        qb::io::uri uri(std::string(c.value), c.af);
        benchmark::DoNotOptimize(uri.is_valid());
        benchmark::DoNotOptimize(uri.host().data());
        benchmark::DoNotOptimize(uri.path().data());
    }

    state.SetLabel(std::string(c.name));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(c.value.size()));
}

void
BM_Uri_ParseQueryHeavy(benchmark::State &state) {
    const auto fields = static_cast<std::size_t>(state.range(0));
    const auto input  = make_query_payload(fields);

    for (auto _ : state) {
        qb::io::uri uri(input);
        benchmark::DoNotOptimize(uri.queries().size());
        benchmark::DoNotOptimize(uri.query("field0").data());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(input.size()));
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(fields));
}

void
BM_Uri_Encode(benchmark::State &state) {
    const auto input = make_encoding_payload(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        auto encoded = qb::io::uri::encode(std::string_view(input));
        benchmark::DoNotOptimize(encoded.data());
        benchmark::DoNotOptimize(encoded.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(input.size()));
}

void
BM_Uri_Decode(benchmark::State &state) {
    const auto input   = make_encoding_payload(static_cast<std::size_t>(state.range(0)));
    const auto encoded = qb::io::uri::encode(std::string_view(input));

    for (auto _ : state) {
        auto decoded = qb::io::uri::decode(std::string_view(encoded));
        benchmark::DoNotOptimize(decoded.data());
        benchmark::DoNotOptimize(decoded.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(encoded.size()));
}

void
BM_Uri_DecodeInvalidEscapes(benchmark::State &state) {
    const auto input = make_invalid_escape_payload(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        auto decoded = qb::io::uri::decode(std::string_view(input));
        benchmark::DoNotOptimize(decoded.data());
        benchmark::DoNotOptimize(decoded.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(input.size()));
}

void
BM_Uri_NormalizePath(benchmark::State &state) {
    const auto  segments = static_cast<std::size_t>(state.range(0));
    std::string base;
    for (std::size_t i = 0; i < segments; ++i)
        base += "/api/./v1/" + std::to_string(i) + "/../resource";

    for (auto _ : state) {
        auto path = base;
        benchmark::DoNotOptimize(qb::io::uri::normalize_path(path));
        benchmark::DoNotOptimize(path.data());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(base.size()));
}

} // namespace

BENCHMARK(BM_Uri_ParseCommon)->DenseRange(0, static_cast<int>(kUriCases.size() - 1))->ArgName("case")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Uri_ParseQueryHeavy)->Args({4})->Args({16})->Args({64})->ArgName("query_fields")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Uri_Encode)->Args({64})->Args({1024})->Args({16 * 1024})->ArgName("bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Uri_Decode)->Args({64})->Args({1024})->Args({16 * 1024})->ArgName("source_bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Uri_DecodeInvalidEscapes)->Args({64})->Args({1024})->Args({16 * 1024})->ArgName("source_bytes")->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Uri_NormalizePath)->Args({4})->Args({32})->Args({128})->ArgName("segments")->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
