/**
 * @file qb/io/tests/benchmark/compression/compress-codecs.cpp
 * @brief Benchmarks for qb compression providers and pipe adapters.
 *
 * Compression is optional and only built when QB_HAS_COMPRESSION is enabled.
 * The scenarios compare gzip and deflate across compressible and mixed data,
 * including one-shot helpers, decompression, and streaming providers; the
 * Codec scenarios run every codec the build registers -- zstd and brotli too,
 * in a build with QB_HAS_ZSTD / QB_HAS_BROTLI -- through the provider path a
 * server pays per response, on JSON and HTML text.
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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <qb/io/compression.h>
#include <qb/system/allocator/pipe.h>

namespace {

enum class Codec : std::uint8_t { Gzip, Deflate };
enum class PayloadKind : std::uint8_t { HttpText, Binary, RepeatedPattern, Json, Html };

// Text with the redundancy of a real response and not more: words drawn from a vocabulary by a fixed generator,
// numbers that vary, markup that repeats. The HttpText and RepeatedPattern shapes repeat one line, which every codec
// reduces to almost nothing, and so cannot rank codecs against each other.
std::string
make_text(std::size_t size, PayloadKind kind) {
    static constexpr std::string_view words[] = {"account", "active",  "address",  "amount",  "archive",  "balance", "billing", "browser",
                                                 "cache",   "channel", "client",   "cluster", "comment",  "config",  "content", "country",
                                                 "created", "credit",  "customer", "default", "delivery", "device",  "display", "domain",
                                                 "draft",   "editor",  "enabled",  "event",   "expires",  "feature", "filter",  "gateway",
                                                 "group",   "history", "invoice",  "label",   "language", "latency", "library", "limit",
                                                 "message", "metric",  "mobile",   "network", "order",    "owner",   "payload", "payment",
                                                 "pending", "profile", "project",  "quota",   "region",   "release", "report",  "request",
                                                 "service", "session", "status",   "storage", "summary",  "ticket",  "update",  "version"};
    std::uint32_t                     x       = 0x9e3779b9u;
    auto                              next    = [&x] {
        x = x * 1664525u + 1013904223u;
        return x >> 8u;
    };
    auto word = [&] {
        return std::string(words[next() % std::size(words)]);
    };
    // A braced list is evaluated left to right; the operands of a `+` chain are not, and g++ and clang draw the
    // generator in different orders there -- the payload, and every ratio, would depend on the compiler.
    auto append = [](std::string &to, std::initializer_list<std::string> pieces) {
        for (const auto &piece : pieces)
            to += piece;
    };

    std::string out;
    out.reserve(size + 512);
    if (kind == PayloadKind::Json) {
        out += "{\"items\":[";
        for (std::uint32_t id = 1; out.size() < size; ++id) {
            append(out, {"{\"id\":",
                         std::to_string(id),
                         ",\"name\":\"",
                         word(),
                         "-",
                         word(),
                         "\",\"email\":\"",
                         word(),
                         std::to_string(next() % 10000),
                         "@example.com\",\"active\":",
                         next() & 1u ? "true" : "false",
                         ",\"score\":",
                         std::to_string(next() % 100000),
                         ",\"tags\":[\"",
                         word(),
                         "\",\"",
                         word(),
                         "\"],\"created\":\"2026-",
                         std::to_string(1 + next() % 12),
                         "-",
                         std::to_string(1 + next() % 28),
                         "T",
                         std::to_string(next() % 24),
                         ":",
                         std::to_string(next() % 60),
                         ":00Z\"},"});
        }
    } else {
        out += "<!doctype html><html><head><title>Items</title></head><body><main>\n";
        for (std::uint32_t id = 1; out.size() < size; ++id) {
            append(out, {"<article class=\"card\" data-id=\"", std::to_string(id), "\"><h2>", word(), " ", word(), "</h2><p>"});
            for (int w = 0; w < 24; ++w) {
                out += word();
                out += ' ';
            }
            append(out, {"</p><a href=\"/", word(), "/", std::to_string(next() % 100000), "\">", word(), "</a></article>\n"});
        }
    }
    out.resize(size);
    return out;
}

std::string
make_payload(std::size_t size, PayloadKind kind) {
    if (kind == PayloadKind::Json || kind == PayloadKind::Html)
        return make_text(size, kind);

    std::string out;
    out.reserve(size);

    if (kind == PayloadKind::HttpText) {
        constexpr std::string_view pattern = "GET /api/resource HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n";
        while (out.size() < size)
            out.append(pattern.data(), std::min(pattern.size(), size - out.size()));
        return out;
    }

    if (kind == PayloadKind::RepeatedPattern) {
        constexpr std::string_view pattern = "The quick brown fox jumps over the lazy dog. QB compression benchmark payload. ";
        while (out.size() < size)
            out.append(pattern.data(), std::min(pattern.size(), size - out.size()));
        return out;
    }

    std::uint32_t x = 0x12345678u;
    for (std::size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        out.push_back(static_cast<char>((x >> 24u) & 0xffu));
    }
    return out;
}

std::string
make_payload(std::size_t size, bool compressible) {
    return make_payload(size, compressible ? PayloadKind::HttpText : PayloadKind::Binary);
}

std::string
compress_one_shot(Codec codec, std::string const &payload, int level) {
    if (codec == Codec::Gzip)
        return qb::gzip::compress(payload.data(), payload.size(), level);
    return qb::deflate::compress(payload.data(), payload.size(), level);
}

std::string
uncompress_one_shot(Codec codec, std::string const &compressed) {
    if (codec == Codec::Gzip)
        return qb::gzip::uncompress(compressed.data(), compressed.size());
    return qb::deflate::uncompress(compressed.data(), compressed.size());
}

void
BM_Compression_Compress(benchmark::State &state, Codec codec, bool compressible) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto level   = static_cast<int>(state.range(1));
    const auto payload = make_payload(size, compressible);

    std::size_t last_compressed = 0;
    for (auto _ : state) {
        auto compressed = compress_one_shot(codec, payload, level);
        last_compressed = compressed.size();
        benchmark::DoNotOptimize(compressed.data());
        benchmark::DoNotOptimize(compressed.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
    if (size)
        state.counters["compress_ratio"] = static_cast<double>(last_compressed) / static_cast<double>(size);
}

void
BM_Compression_Uncompress(benchmark::State &state, Codec codec, bool compressible) {
    const auto size       = static_cast<std::size_t>(state.range(0));
    const auto payload    = make_payload(size, compressible);
    const auto compressed = compress_one_shot(codec, payload, Z_DEFAULT_COMPRESSION);

    std::string last;
    for (auto _ : state) {
        last = uncompress_one_shot(codec, compressed);
        benchmark::DoNotOptimize(last.data());
        benchmark::DoNotOptimize(last.size());
    }

    // Out-of-loop correctness assert: decompression must reproduce the payload.
    if (last != payload)
        state.SkipWithError("uncompress did not round-trip the payload");

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_Compression_PipeAdapter(benchmark::State &state, Codec codec) {
    const auto                size    = static_cast<std::size_t>(state.range(0));
    const auto                payload = make_payload(size, true);
    qb::allocator::pipe<char> pipe;

    for (auto _ : state) {
        pipe.reset();
        if (codec == Codec::Gzip) {
            qb::gzip::to_compress info{payload.data(), payload.size()};
            pipe << info;
            benchmark::DoNotOptimize(info.size_compressed);
        } else {
            qb::deflate::to_compress info{payload.data(), payload.size()};
            pipe << info;
            benchmark::DoNotOptimize(info.size_compressed);
        }
        benchmark::DoNotOptimize(pipe.begin());
        benchmark::DoNotOptimize(pipe.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_Compression_StreamingProvider(benchmark::State &state, Codec codec) {
    const auto size       = static_cast<std::size_t>(state.range(0));
    const auto chunk_size = static_cast<std::size_t>(state.range(1));
    const auto payload    = make_payload(size, false);

    // Hoist compressor construction OUT of the timed loop: make_compressor()
    // allocates a zlib z_stream + ~256 KiB window/dictionary, which would
    // otherwise dominate the per-iteration cost. We reset() its internal state
    // at the top of each iteration instead — the steady-state cost a long-lived
    // streaming session actually pays.
    auto compressor = qb::compression::builtin::make_compressor(codec == Codec::Gzip ? "gzip" : "deflate");
    if (!compressor) {
        state.SkipWithError("make_compressor returned null");
        return;
    }

    const auto  out_block       = std::max<std::size_t>(chunk_size * 2u, 256u);
    std::size_t last_compressed = 0;
    for (auto _ : state) {
        compressor->reset();
        qb::allocator::pipe<char> out;
        std::size_t               input_offset = 0;
        bool                      done         = false;

        while (!done) {
            const auto  remaining = payload.size() - input_offset;
            const auto  in_size   = std::min(chunk_size, remaining);
            auto       *dst       = reinterpret_cast<std::uint8_t *>(out.allocate_back(out_block));
            std::size_t consumed  = 0;
            const auto  produced  = compressor->compress(
                reinterpret_cast<const std::uint8_t *>(payload.data() + input_offset), in_size, dst, out_block,
                input_offset + in_size == payload.size() ? qb::compression::is_last : qb::compression::has_more, consumed, done);
            input_offset += consumed;
            out.free_back(out_block - produced);
        }

        last_compressed = out.size();
        benchmark::DoNotOptimize(out.begin());
        benchmark::DoNotOptimize(out.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
    // Ratio column: compressed / original for this codec+payload (lower is better).
    if (size)
        state.counters["compress_ratio"] = static_cast<double>(last_compressed) / static_cast<double>(size);
}

void
BM_Compression_DataShape(benchmark::State &state, Codec codec, PayloadKind kind) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size, kind);

    std::size_t last_compressed = 0;
    for (auto _ : state) {
        auto compressed = compress_one_shot(codec, payload, Z_DEFAULT_COMPRESSION);
        last_compressed = compressed.size();
        benchmark::DoNotOptimize(compressed.data());
        benchmark::DoNotOptimize(compressed.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
    // The whole point of this bench is comparing ratio across data shapes.
    if (size)
        state.counters["compress_ratio"] = static_cast<double>(last_compressed) / static_cast<double>(size);
}

// One whole stream through a provider made by name, as qbm-http's Body::compress makes it; 0 if the window was too
// small to finish (the caller reports it rather than timing a partial stream).
std::size_t
compress_stream(qb::compression::compress_provider &c, std::string const &in, std::vector<std::uint8_t> &out) {
    std::size_t consumed = 0, produced = 0;
    bool        done = false;
    while (!done && produced < out.size()) {
        std::size_t used = 0;
        produced += c.compress(reinterpret_cast<const std::uint8_t *>(in.data()) + consumed, in.size() - consumed, out.data() + produced,
                               out.size() - produced, qb::compression::is_last, used, done);
        consumed += used;
    }
    return done ? produced : 0;
}

void
BM_Compression_Codec(benchmark::State &state, const char *algorithm, PayloadKind kind) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size, kind);
    if (!qb::compression::builtin::make_compressor(algorithm)) {
        state.SkipWithError("codec not registered in this build");
        return;
    }
    std::vector<std::uint8_t> out(size + size / 2 + 1024);
    std::size_t               last_compressed = 0;
    // A provider per stream, as Body::compress makes one per response: its allocation is part of what a server pays,
    // and it is not the same for every codec on every allocator.
    for (auto _ : state) {
        auto compressor = qb::compression::builtin::make_compressor(algorithm);
        last_compressed = compress_stream(*compressor, payload, out);
        benchmark::DoNotOptimize(out.data());
    }
    if (!last_compressed) {
        state.SkipWithError("the output window did not hold the whole stream");
        return;
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
    state.counters["compress_ratio"] = static_cast<double>(last_compressed) / static_cast<double>(size);
}

void
BM_Compression_CodecUncompress(benchmark::State &state, const char *algorithm, PayloadKind kind) {
    const auto size       = static_cast<std::size_t>(state.range(0));
    const auto payload    = make_payload(size, kind);
    auto       compressor = qb::compression::builtin::make_compressor(algorithm);
    if (!compressor || !qb::compression::builtin::make_decompressor(algorithm)) {
        state.SkipWithError("codec not registered in this build");
        return;
    }
    std::vector<std::uint8_t> compressed(size + size / 2 + 1024);
    compressed.resize(compress_stream(*compressor, payload, compressed));

    // Room past the payload: a decoder may need one more call, after the last byte, to read its trailer and say done.
    std::vector<std::uint8_t> out(size + 64);
    std::size_t               produced = 0;
    bool                      done     = false;
    for (auto _ : state) {
        auto        decompressor = qb::compression::builtin::make_decompressor(algorithm); // one per body, as Body::uncompress
        std::size_t consumed     = 0;
        produced                 = 0;
        done                     = false;
        while (!done && produced < out.size()) {
            std::size_t used = 0;
            produced += decompressor->decompress(compressed.data() + consumed, compressed.size() - consumed, out.data() + produced,
                                                 out.size() - produced, qb::compression::is_last, used, done);
            consumed += used;
        }
        benchmark::DoNotOptimize(out.data());
    }
    // Out-of-loop correctness assert: decompression must reproduce the payload.
    if (!done || produced != size || std::memcmp(out.data(), payload.data(), size) != 0) {
        state.SkipWithError("the codec did not round-trip the payload");
        return;
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

} // namespace

BENCHMARK_CAPTURE(BM_Compression_Compress, gzip_compressible, Codec::Gzip, true)
    ->Args({4 * 1024, Z_BEST_SPEED})
    ->Args({64 * 1024, Z_BEST_SPEED})
    ->Args({64 * 1024, Z_DEFAULT_COMPRESSION})
    ->Args({64 * 1024, Z_BEST_COMPRESSION})
    ->Args({1024 * 1024, Z_DEFAULT_COMPRESSION})
    ->ArgNames({"bytes", "level"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_Compress, deflate_compressible, Codec::Deflate, true)
    ->Args({4 * 1024, Z_BEST_SPEED})
    ->Args({64 * 1024, Z_BEST_SPEED})
    ->Args({64 * 1024, Z_DEFAULT_COMPRESSION})
    ->Args({64 * 1024, Z_BEST_COMPRESSION})
    ->Args({1024 * 1024, Z_DEFAULT_COMPRESSION})
    ->ArgNames({"bytes", "level"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_Compress, gzip_mixed, Codec::Gzip, false)
    ->Args({4 * 1024, Z_DEFAULT_COMPRESSION})
    ->Args({64 * 1024, Z_DEFAULT_COMPRESSION})
    ->ArgNames({"bytes", "level"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_Compress, deflate_mixed, Codec::Deflate, false)
    ->Args({4 * 1024, Z_DEFAULT_COMPRESSION})
    ->Args({64 * 1024, Z_DEFAULT_COMPRESSION})
    ->ArgNames({"bytes", "level"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_Uncompress, gzip_compressible, Codec::Gzip, true)
    ->Args({4 * 1024})
    ->Args({64 * 1024})
    ->Args({1024 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_Uncompress, deflate_compressible, Codec::Deflate, true)
    ->Args({4 * 1024})
    ->Args({64 * 1024})
    ->Args({1024 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_PipeAdapter, gzip, Codec::Gzip)
    ->Args({4 * 1024})
    ->Args({64 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_PipeAdapter, deflate, Codec::Deflate)
    ->Args({4 * 1024})
    ->Args({64 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_StreamingProvider, gzip, Codec::Gzip)
    ->Args({64 * 1024, 1024})
    ->Args({1024 * 1024, 16 * 1024})
    ->ArgNames({"bytes", "chunk_bytes"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_StreamingProvider, deflate, Codec::Deflate)
    ->Args({64 * 1024, 1024})
    ->Args({1024 * 1024, 16 * 1024})
    ->ArgNames({"bytes", "chunk_bytes"})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, gzip_http_text, Codec::Gzip, PayloadKind::HttpText)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, gzip_binary, Codec::Gzip, PayloadKind::Binary)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, gzip_repeated_pattern, Codec::Gzip, PayloadKind::RepeatedPattern)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, deflate_http_text, Codec::Deflate, PayloadKind::HttpText)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, deflate_binary, Codec::Deflate, PayloadKind::Binary)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_Compression_DataShape, deflate_repeated_pattern, Codec::Deflate, PayloadKind::RepeatedPattern)
    ->Args({256 * 1024})
    ->ArgName("bytes")
    ->Unit(benchmark::kMicrosecond);

// Every codec this build registers, on the two text shapes a server answers with most, at a small, a typical and a
// large response: the speed and the ratio that rank them in qbm-http's CompressionOptions preference list.
#define QB_BENCH_CODEC(name, algorithm)                                                          \
    BENCHMARK_CAPTURE(BM_Compression_Codec, name##_json, algorithm, PayloadKind::Json)           \
        ->Args({4 * 1024})                                                                       \
        ->Args({64 * 1024})                                                                      \
        ->Args({1024 * 1024})                                                                    \
        ->ArgName("bytes")                                                                       \
        ->Unit(benchmark::kMicrosecond);                                                         \
    BENCHMARK_CAPTURE(BM_Compression_Codec, name##_html, algorithm, PayloadKind::Html)           \
        ->Args({4 * 1024})                                                                       \
        ->Args({64 * 1024})                                                                      \
        ->Args({1024 * 1024})                                                                    \
        ->ArgName("bytes")                                                                       \
        ->Unit(benchmark::kMicrosecond);                                                         \
    BENCHMARK_CAPTURE(BM_Compression_CodecUncompress, name##_json, algorithm, PayloadKind::Json) \
        ->Args({64 * 1024})                                                                      \
        ->Args({1024 * 1024})                                                                    \
        ->ArgName("bytes")                                                                       \
        ->Unit(benchmark::kMicrosecond)

QB_BENCH_CODEC(gzip, "gzip");
QB_BENCH_CODEC(deflate, "deflate");
#if defined(QB_HAS_ZSTD)
QB_BENCH_CODEC(zstd, "zstd");
#endif
#if defined(QB_HAS_BROTLI)
QB_BENCH_CODEC(br, "br");
#endif

BENCHMARK_MAIN();
