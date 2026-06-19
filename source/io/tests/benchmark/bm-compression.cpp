/**
 * @file qb/io/tests/benchmark/bm-compression.cpp
 * @brief Benchmarks for qb compression providers and pipe adapters.
 *
 * Compression is optional and only built when QB_HAS_COMPRESSION is enabled.
 * The scenarios compare gzip and deflate across compressible and mixed data,
 * including one-shot helpers, decompression, and streaming providers.
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
#include <string>
#include <string_view>

#include <qb/io/compression.h>
#include <qb/system/allocator/pipe.h>

namespace {

enum class Codec : std::uint8_t { Gzip, Deflate };
enum class PayloadKind : std::uint8_t { HttpText, Binary, RepeatedPattern };

std::string
make_payload(std::size_t size, PayloadKind kind) {
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

    for (auto _ : state) {
        auto compressed = compress_one_shot(codec, payload, level);
        benchmark::DoNotOptimize(compressed.data());
        benchmark::DoNotOptimize(compressed.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_Compression_Uncompress(benchmark::State &state, Codec codec, bool compressible) {
    const auto size       = static_cast<std::size_t>(state.range(0));
    const auto payload    = make_payload(size, compressible);
    const auto compressed = compress_one_shot(codec, payload, Z_DEFAULT_COMPRESSION);

    for (auto _ : state) {
        auto uncompressed = uncompress_one_shot(codec, compressed);
        benchmark::DoNotOptimize(uncompressed.data());
        benchmark::DoNotOptimize(uncompressed.size());
    }

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

    for (auto _ : state) {
        auto                      compressor = qb::compression::builtin::make_compressor(codec == Codec::Gzip ? "gzip" : "deflate");
        qb::allocator::pipe<char> out;
        std::size_t               input_offset = 0;
        bool                      done         = false;

        while (!done) {
            const auto  remaining = payload.size() - input_offset;
            const auto  in_size   = std::min(chunk_size, remaining);
            auto       *dst       = reinterpret_cast<std::uint8_t *>(out.allocate_back(std::max<std::size_t>(chunk_size * 2u, 256u)));
            std::size_t consumed  = 0;
            const auto  produced  = compressor->compress(
                reinterpret_cast<const std::uint8_t *>(payload.data() + input_offset), in_size, dst,
                std::max<std::size_t>(chunk_size * 2u, 256u),
                input_offset + in_size == payload.size() ? qb::compression::is_last : qb::compression::has_more, consumed, done);
            input_offset += consumed;
            out.free_back(std::max<std::size_t>(chunk_size * 2u, 256u) - produced);
        }

        benchmark::DoNotOptimize(out.begin());
        benchmark::DoNotOptimize(out.size());
    }

    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(size));
}

void
BM_Compression_DataShape(benchmark::State &state, Codec codec, PayloadKind kind) {
    const auto size    = static_cast<std::size_t>(state.range(0));
    const auto payload = make_payload(size, kind);

    for (auto _ : state) {
        auto compressed = compress_one_shot(codec, payload, Z_DEFAULT_COMPRESSION);
        benchmark::DoNotOptimize(compressed.data());
        benchmark::DoNotOptimize(compressed.size());
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

BENCHMARK_MAIN();
