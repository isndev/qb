/**
 * @file unit/compression/compression-codec.cpp
 * @brief qb::compression gzip/deflate codecs - single-shot + streaming. Link-gated on QB_HAS_COMPRESSION.
 *
 * Tests the compression/decompression API (qb/io/compression.h): gzip + deflate, single-operation
 * round-trips and streaming providers. Pure codec logic, no engine/IO - a strict unit test.
 * Renamed from system/test-compression.cpp and registered (was orphaned: present but unwired).
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

#include <gtest/gtest.h>
#include <qb/io/compression.h>
#include <qb/io/crypto.h>
#include <qb/system/allocator/pipe.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "codec_contract.h"

TEST(Compression, Gzip) {
    auto                      compressor   = qb::compression::builtin::make_compressor("gzip");
    auto                      decompressor = qb::compression::builtin::make_decompressor("gzip");
    auto                      from         = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> buffer;
    buffer.allocate_back(128000);

    std::size_t i_processed{};
    bool        done{};
    auto        o_processed =
        compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str()), from.size(), reinterpret_cast<uint8_t *>(buffer.begin()),
                             buffer.size(), qb::compression::is_last, i_processed, done);
    buffer.free_back(buffer.size() - o_processed);
    EXPECT_TRUE(done);
    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed =
        decompressor->decompress(reinterpret_cast<uint8_t const *>(buffer.begin()), buffer.size(), reinterpret_cast<uint8_t *>(buffer2.begin()),
                                 buffer2.size(), qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::gzip::uncompress(buffer.begin(), buffer.size()));
}

TEST(Compression, Gzip_Stream) {
    auto                      compressor   = qb::compression::builtin::make_compressor("gzip");
    auto                      decompressor = qb::compression::builtin::make_decompressor("gzip");
    auto                      from         = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> i_buffer, o_buffer;
    i_buffer.allocate_back(128000);

    bool        done{};
    std::size_t o_processed{}, i_processed{};
    while (!done) {
        auto        out = o_buffer.allocate_back(100);
        std::size_t ci_processed{};
        o_processed +=
            compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str() + i_processed), from.size() - i_processed,
                                 reinterpret_cast<uint8_t *>(out), o_buffer.size() - o_processed, qb::compression::is_last, ci_processed, done);
        i_processed += ci_processed;
    }

    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed =
        decompressor->decompress(reinterpret_cast<uint8_t const *>(o_buffer.begin()), o_buffer.size(),
                                 reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(), qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::gzip::uncompress(o_buffer.begin(), o_buffer.size()));
}

TEST(Compression, Deflate) {
    auto                      compressor   = qb::compression::builtin::make_compressor("deflate");
    auto                      decompressor = qb::compression::builtin::make_decompressor("deflate");
    auto                      from         = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> buffer;
    buffer.allocate_back(128000);

    std::size_t i_processed{};
    bool        done{};
    auto        o_processed =
        compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str()), from.size(), reinterpret_cast<uint8_t *>(buffer.begin()),
                             buffer.size(), qb::compression::is_last, i_processed, done);
    buffer.free_back(buffer.size() - o_processed);
    EXPECT_TRUE(done);
    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed =
        decompressor->decompress(reinterpret_cast<uint8_t const *>(buffer.begin()), buffer.size(), reinterpret_cast<uint8_t *>(buffer2.begin()),
                                 buffer2.size(), qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::deflate::uncompress(buffer.begin(), buffer.size()));
}

TEST(Compression, Deflate_Stream) {
    auto                      compressor   = qb::compression::builtin::make_compressor("deflate");
    auto                      decompressor = qb::compression::builtin::make_decompressor("deflate");
    auto                      from         = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> i_buffer, o_buffer;
    i_buffer.allocate_back(128000);

    bool        done{};
    std::size_t o_processed{}, i_processed{};
    while (!done) {
        auto        out = o_buffer.allocate_back(100);
        std::size_t ci_processed{};
        o_processed +=
            compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str() + i_processed), from.size() - i_processed,
                                 reinterpret_cast<uint8_t *>(out), o_buffer.size() - o_processed, qb::compression::is_last, ci_processed, done);
        i_processed += ci_processed;
    }

    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed =
        decompressor->decompress(reinterpret_cast<uint8_t const *>(o_buffer.begin()), o_buffer.size(),
                                 reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(), qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::deflate::uncompress(o_buffer.begin(), o_buffer.size()));
}

TEST(Compression, Gzip_All) {
    std::string from           = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    std::string compressed_str = qb::gzip::compress(from.c_str(), from.size());
    EXPECT_EQ(from, qb::gzip::uncompress(compressed_str.c_str(), compressed_str.size()));

    qb::allocator::pipe<char> compressed_pipe;
    qb::gzip::to_compress     to_c{from.c_str(), from.size()};
    compressed_pipe << to_c;
    EXPECT_EQ(compressed_str.size(), to_c.size_compressed);
    EXPECT_EQ(compressed_str.size(), compressed_pipe.size());
    EXPECT_EQ(compressed_str, std::string(compressed_pipe.begin(), compressed_pipe.size()));

    qb::gzip::to_uncompress   to_uc{compressed_pipe.begin(), compressed_pipe.size()};
    qb::allocator::pipe<char> uncompressed_pipe;
    uncompressed_pipe << to_uc;
    EXPECT_EQ(from.size(), to_uc.size_uncompressed);
    EXPECT_EQ(from.size(), uncompressed_pipe.size());
    EXPECT_EQ(from, std::string(uncompressed_pipe.begin(), uncompressed_pipe.size()));
}

TEST(Compression, Deflate_All) {
    std::string from           = qb::crypto::generate_random_string(128000, qb::crypto::range_alpha_numeric_special);
    std::string compressed_str = qb::deflate::compress(from.c_str(), from.size());
    EXPECT_EQ(from, qb::deflate::uncompress(compressed_str.c_str(), compressed_str.size()));

    qb::allocator::pipe<char> compressed_pipe;
    qb::deflate::to_compress  to_c{from.c_str(), from.size()};
    compressed_pipe << to_c;
    EXPECT_EQ(compressed_str.size(), to_c.size_compressed);
    EXPECT_EQ(compressed_str.size(), compressed_pipe.size());
    EXPECT_EQ(compressed_str, std::string(compressed_pipe.begin(), compressed_pipe.size()));

    qb::deflate::to_uncompress to_uc{compressed_pipe.begin(), compressed_pipe.size()};
    qb::allocator::pipe<char>  uncompressed_pipe;
    uncompressed_pipe << to_uc;
    EXPECT_EQ(from.size(), to_uc.size_uncompressed);
    EXPECT_EQ(from.size(), uncompressed_pipe.size());
    EXPECT_EQ(from, std::string(uncompressed_pipe.begin(), uncompressed_pipe.size()));
}

// Decompression-bomb guard: a small input that expands beyond `max` must throw
// instead of allocating unboundedly.
TEST(Compression, DecompressionBombBoundedByMax) {
    std::string original(4 * 1024 * 1024, '\0'); // 4 MiB, compresses tiny
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_LT(compressed.size(), original.size());

    // 64 KiB output budget → decompression MUST be rejected.
    qb::allocator::pipe<char> out;
    EXPECT_THROW(qb::gzip::uncompress(out, compressed.c_str(), compressed.size(), static_cast<std::size_t>(64 * 1024)), std::runtime_error);

    // Generous budget → succeeds and round-trips.
    qb::allocator::pipe<char> out_ok;
    EXPECT_NO_THROW(qb::gzip::uncompress(out_ok, compressed.c_str(), compressed.size(), static_cast<std::size_t>(8 * 1024 * 1024)));
    EXPECT_EQ(out_ok.size(), original.size());
}

// Truncated stream must be rejected (no silent partial output).
TEST(Compression, TruncatedStreamRejected) {
    std::string original   = qb::crypto::generate_random_string(100000, qb::crypto::range_alpha_numeric_special);
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_GT(compressed.size(), 16u);

    // Lop off the tail: the stream can no longer reach Z_STREAM_END.
    std::string               truncated = compressed.substr(0, compressed.size() - 8);
    qb::allocator::pipe<char> out;
    EXPECT_THROW(qb::gzip::uncompress(out, truncated.c_str(), truncated.size()), std::runtime_error);
}

// The generic `compression::uncompress<Output>` template (resize()/operator[] container path) is a
// DISTINCT codepath from the `qb::allocator::pipe<char>` specialization exercised above. These three
// cases drive that template with a std::string output through gzip::uncompress(std::string&, ...),
// hitting the two decompression-bomb guards and the truncated-stream guard inside the template.

// Generic-template guard #1: the up-front `max && size > max/2` budget check (before the inflate
// loop). A `max` smaller than 2x the compressed input must be rejected immediately.
TEST(Compression, GenericTemplateUpfrontBudgetGuardRejects) {
    std::string original(2 * 1024 * 1024, '\0'); // compresses to a small S
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_GT(compressed.size(), 2u);

    // max = 1: 1 && compressed.size() > 0 -> throws "size may use more memory than intended".
    std::string out;
    EXPECT_THROW(qb::gzip::uncompress(out, compressed.c_str(), compressed.size(), static_cast<std::size_t>(1)), std::runtime_error);
    EXPECT_TRUE(out.empty() || out.size() <= 1u);
}

// Generic-template guard #2: the in-loop decompression-bomb guard. `max` is chosen to PASS the
// up-front check (max >= 2*compressed) yet be exceeded once the inflate loop expands the 2 MiB of
// zeros past it — exercising the throw inside the do/while, not the up-front guard.
TEST(Compression, GenericTemplateInLoopBombGuardRejects) {
    std::string original(2 * 1024 * 1024, '\0');
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_GT(compressed.size(), 2u);
    ASSERT_LT(compressed.size(), original.size());

    // max = 3*S: passes (S <= max/2) but the decompressed 2 MiB blows past 3*S in the loop.
    const std::size_t max = 3u * compressed.size();
    std::string       out;
    EXPECT_THROW(qb::gzip::uncompress(out, compressed.c_str(), compressed.size(), max), std::runtime_error);

    // A generous budget through the SAME generic template round-trips the full payload.
    std::string out_ok;
    EXPECT_NO_THROW(qb::gzip::uncompress(out_ok, compressed.c_str(), compressed.size(), static_cast<std::size_t>(8 * 1024 * 1024)));
    EXPECT_EQ(out_ok.size(), original.size());
    EXPECT_EQ(out_ok, original);
}

// Generic-template guard #3: a truncated stream (unbounded max, so both bomb guards are skipped)
// must be rejected by the "incomplete or truncated compressed stream" check after the loop, never
// returning silent partial output.
TEST(Compression, GenericTemplateTruncatedStreamRejected) {
    std::string original   = qb::crypto::generate_random_string(80000, qb::crypto::range_alpha_numeric_special);
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_GT(compressed.size(), 16u);

    std::string truncated = compressed.substr(0, compressed.size() - 8);
    std::string out;
    EXPECT_THROW(qb::gzip::uncompress(out, truncated.c_str(), truncated.size()), std::runtime_error);

    // Deflate's generic template wrapper shares the same template; the intact stream round-trips.
    std::string zlib = qb::deflate::compress(original.c_str(), original.size());
    std::string out_ok;
    EXPECT_NO_THROW(qb::deflate::uncompress(out_ok, zlib.c_str(), zlib.size()));
    EXPECT_EQ(out_ok, original);
}

// gzip and deflate under the same provider contract as the opt-in codecs (codec_contract.h): any input piece and
// output window, a truncated stream never done, a corrupt one refused, reset() reusable -- and the decompressor hands
// back on an empty call what an earlier call had no room for (Huly QB-464).
TEST(Compression, GzipAndDeflateHonourTheProviderContract) {
    namespace contract = qb::io::test::codec_contract;
    for (const char *name : {qb::compression::builtin::algorithm::GZIP, qb::compression::builtin::algorithm::DEFLATE}) {
        contract::expect_round_trips(name);
        contract::expect_truncation_never_done_and_corruption_throws(name);
        contract::expect_reset_reuses(name);
    }
}

// A partial flush that did not fit its window is drained by the calls without input that follow: those calls returned
// nothing, and the flush stayed inside zlib until the stream was finished (Huly QB-355). A 512-byte window: zlib's own
// caveat on tiny drain windows is in the helper.
TEST(Compression, GzipAndDeflateDrainAFlushThatDidNotFitOnAnEmptyCall) {
    namespace contract = qb::io::test::codec_contract;
    for (const char *name : {qb::compression::builtin::algorithm::GZIP, qb::compression::builtin::algorithm::DEFLATE})
        contract::expect_empty_continuation_drains(name, 512);
}

TEST(Compression, BuiltinFactoriesAndAlgorithms) {
    namespace builtin = qb::compression::builtin;

    EXPECT_TRUE(builtin::supported());
    EXPECT_TRUE(builtin::algorithm::supported("gzip"));
    EXPECT_TRUE(builtin::algorithm::supported("GZIP"));
    EXPECT_TRUE(builtin::algorithm::supported("DefLate"));
#if defined(QB_HAS_BROTLI)
    EXPECT_TRUE(builtin::algorithm::supported("br")); // the opt-in codec (Huly QB-79): compression-brotli.cpp holds it
#else
    EXPECT_FALSE(builtin::algorithm::supported("br"));
#endif
    EXPECT_FALSE(builtin::algorithm::supported("lzma")); // a name no build registers

    const auto compressors   = builtin::get_compress_factories();
    const auto decompressors = builtin::get_decompress_factories();
    ASSERT_GE(compressors.size(), 2u);
    ASSERT_GE(decompressors.size(), 2u);

    auto gzip_factory = builtin::get_compress_factory("GzIp");
    ASSERT_NE(gzip_factory, nullptr);
    EXPECT_EQ(gzip_factory->algorithm(), "gzip");
    ASSERT_NE(gzip_factory->make_compressor(), nullptr);

    auto deflate_factory = builtin::get_decompress_factory("DEFLATE");
    ASSERT_NE(deflate_factory, nullptr);
    EXPECT_EQ(deflate_factory->algorithm(), "deflate");
    EXPECT_EQ(deflate_factory->weight(), 500u);
    ASSERT_NE(deflate_factory->make_decompressor(), nullptr);

    EXPECT_EQ(builtin::make_compressor("missing"), nullptr);
    EXPECT_EQ(builtin::make_decompressor("missing"), nullptr);
    EXPECT_EQ(builtin::get_compress_factory("missing"), nullptr);
    EXPECT_EQ(builtin::get_decompress_factory("missing"), nullptr);

    auto custom_compress = qb::compression::make_compress_factory("custom", [] { return nullptr; });
    ASSERT_NE(custom_compress, nullptr);
    EXPECT_EQ(custom_compress->algorithm(), "custom");
    EXPECT_EQ(custom_compress->make_compressor(), nullptr);

    auto custom_decompress = qb::compression::make_decompress_factory("custom", 7u, [] { return nullptr; });
    ASSERT_NE(custom_decompress, nullptr);
    EXPECT_EQ(custom_decompress->algorithm(), "custom");
    EXPECT_EQ(custom_decompress->weight(), 7u);
    EXPECT_EQ(custom_decompress->make_decompressor(), nullptr);
}

TEST(Compression, ProvidersHandleStreamingResetAndFinishedState) {
    namespace builtin = qb::compression::builtin;

    const std::string    input = "qb compression streaming reset contract " + std::string(4096, 'x');
    std::vector<uint8_t> compressed(input.size() + 256);
    std::vector<uint8_t> restored(input.size() + 16);

    auto compressor = builtin::make_compressor("gzip");
    ASSERT_NE(compressor, nullptr);
    EXPECT_EQ(compressor->algorithm(), "gzip");

    std::size_t processed = 123u;
    bool        done      = true;
    EXPECT_EQ(compressor->compress(reinterpret_cast<const uint8_t *>(input.data()), 0, compressed.data(), compressed.size(),
                                   qb::compression::has_more, processed, done),
              0u);
    EXPECT_EQ(processed, 0u);
    EXPECT_FALSE(done);

    processed              = 0u;
    done                   = false;
    const auto tiny_output = compressor->compress(reinterpret_cast<const uint8_t *>(input.data()), input.size(), compressed.data(), 1,
                                                  qb::compression::is_last, processed, done);
    EXPECT_LE(tiny_output, 1u);
    EXPECT_FALSE(done);

    compressor->reset();
    processed                  = 0u;
    done                       = false;
    const auto compressed_size = compressor->compress(reinterpret_cast<const uint8_t *>(input.data()), input.size(), compressed.data(),
                                                      compressed.size(), qb::compression::is_last, processed, done);
    EXPECT_TRUE(done);
    EXPECT_EQ(processed, input.size());
    ASSERT_GT(compressed_size, 0u);

    processed = 99u;
    done      = false;
    EXPECT_EQ(compressor->compress(reinterpret_cast<const uint8_t *>(input.data()), input.size(), compressed.data(), compressed.size(),
                                   qb::compression::is_last, processed, done),
              0u);
    EXPECT_EQ(processed, 0u);
    EXPECT_TRUE(done);

    auto decompressor = builtin::make_decompressor("gzip");
    ASSERT_NE(decompressor, nullptr);
    EXPECT_EQ(decompressor->algorithm(), "gzip");

    processed = 42u;
    done      = true;
    EXPECT_EQ(decompressor->decompress(compressed.data(), 0, restored.data(), restored.size(), qb::compression::is_last, processed, done), 0u);
    EXPECT_EQ(processed, 0u);
    EXPECT_FALSE(done);

    processed = 0u;
    done      = false;
    const auto partial =
        decompressor->decompress(compressed.data(), compressed_size, restored.data(), 1, qb::compression::is_last, processed, done);
    EXPECT_LE(partial, 1u);
    EXPECT_FALSE(done);

    decompressor->reset();
    processed                = 0u;
    done                     = false;
    const auto restored_size = decompressor->decompress(compressed.data(), compressed_size, restored.data(), restored.size(),
                                                        qb::compression::is_last, processed, done);
    EXPECT_TRUE(done);
    EXPECT_EQ(processed, compressed_size);
    EXPECT_EQ(restored_size, input.size());
    EXPECT_EQ(std::string(reinterpret_cast<char *>(restored.data()), restored_size), input);

    processed = 1u;
    done      = false;
    EXPECT_EQ(decompressor->decompress(compressed.data(), compressed_size, restored.data(), restored.size(), qb::compression::is_last,
                                       processed, done),
              0u);
    EXPECT_EQ(processed, 0u);
    EXPECT_TRUE(done);
}

TEST(Compression, ErrorAndDetectionContracts) {
    namespace builtin = qb::compression::builtin;

    EXPECT_THROW(builtin::make_gzip_compressor(-42, Z_DEFLATED, Z_DEFAULT_STRATEGY, 8), std::runtime_error);
    EXPECT_THROW(builtin::make_deflate_compressor(Z_DEFAULT_COMPRESSION, 0, Z_DEFAULT_STRATEGY, 8), std::runtime_error);

    std::string generic_output;
    EXPECT_THROW(qb::compression::compress(generic_output, "qb", 2, Z_DEFAULT_COMPRESSION, 0), std::runtime_error);
    EXPECT_THROW(qb::compression::uncompress(generic_output, "qb", 2, 0, 0), std::runtime_error);

    const std::string input = "detect compression headers";
    const auto        gzip  = qb::gzip::compress(input.data(), input.size());
    const auto        zlib  = qb::deflate::compress(input.data(), input.size());

    EXPECT_TRUE(qb::gzip::is_compressed(gzip.data(), gzip.size()));
    EXPECT_TRUE(qb::gzip::is_compressed(zlib.data(), zlib.size()));
    EXPECT_TRUE(qb::gzip::is_compressed("\x78\x01x", 3));
    EXPECT_TRUE(qb::gzip::is_compressed("\x78\xDAx", 3));
    EXPECT_TRUE(qb::gzip::is_compressed("\x78\x5Ex", 3));
    EXPECT_FALSE(qb::gzip::is_compressed(input.data(), input.size()));
    EXPECT_FALSE(qb::gzip::is_compressed("", 0));

    qb::allocator::pipe<char> empty_output;
    EXPECT_EQ(qb::gzip::uncompress(empty_output, "", 0), 0u);
    EXPECT_EQ(empty_output.size(), 0u);

    qb::allocator::pipe<char> invalid_gzip_output;
    EXPECT_THROW(qb::gzip::uncompress(invalid_gzip_output, input.data(), input.size()), std::runtime_error);

    qb::allocator::pipe<char> too_small_budget_output;
    EXPECT_THROW(qb::gzip::uncompress(too_small_budget_output, gzip.data(), gzip.size(), 1), std::runtime_error);

    qb::allocator::pipe<char> invalid_deflate_output;
    EXPECT_THROW(qb::deflate::uncompress(invalid_deflate_output, input.data(), input.size()), std::runtime_error);

    auto decompressor = builtin::make_decompressor("gzip");
    ASSERT_NE(decompressor, nullptr);
    std::vector<uint8_t> out(64);
    std::size_t          processed = 0u;
    bool                 done      = false;
    EXPECT_THROW(decompressor->decompress(reinterpret_cast<const uint8_t *>(input.data()), input.size(), out.data(), out.size(),
                                          qb::compression::is_last, processed, done),
                 std::runtime_error);
}

// WAVE-3: the pipe<char> SPECIALIZATIONS of compression::compress/uncompress
// (compression.cpp ~481/538) have their own deflateInit2/inflateInit2 failure
// throws (src 506-508 / 565-567). The existing ErrorAndDetectionContracts cases
// drive the std::string Output TEMPLATE (header-inline) path, so the .cpp
// pipe-specialization init-failure throws stay uncovered. An out-of-range
// windowBits (7 is below zlib's 8..15 floor for both deflate and inflate) forces
// Z_STREAM_ERROR from the init call, exercising those specialization throws.
TEST(Compression, PipeSpecializationInitFailureThrows) {
    const std::string         payload = "qb pipe-specialization init-failure path";
    qb::allocator::pipe<char> compress_out;
    // deflateInit2 with windowBits=7 -> Z_STREAM_ERROR -> "deflate init failed".
    EXPECT_THROW(qb::compression::compress(compress_out, payload.data(), payload.size(), Z_DEFAULT_COMPRESSION, /*window_bits*/ 7),
                 std::runtime_error);

    qb::allocator::pipe<char> uncompress_out;
    // inflateInit2 with windowBits=7 -> Z_STREAM_ERROR -> "inflate init failed".
    // size must be non-zero so the early `size == 0` return does not pre-empt init.
    EXPECT_THROW(qb::compression::uncompress(uncompress_out, payload.data(), payload.size(), /*max*/ 0u, /*window_bits*/ 7),
                 std::runtime_error);
}

// =============================================================================
// Empty input must TERMINATE (regression: infinite 100%-CPU loop)
// =============================================================================

/**
 * @test uncompress() with size == 0 returns immediately instead of spinning forever
 * @brief Regression (hang, found by fuzzing): the generic
 *        `template <typename Output> compression::uncompress(...)` lacked the `size == 0`
 *        early return that the `pipe<char>` specialisation has. With size 0 the decode loop
 *        computes `chunk = 2 * size == 0`, so `avail_out` is 0 on entry, `inflate()` can make
 *        no progress and returns Z_BUF_ERROR (an accepted status), `size_uncompressed` never
 *        advances, and `while (avail_out == 0)` never exits — an unkillable 100%-CPU spin on
 *        the calling (event-loop) thread that allocates nothing, so no memory limit ever trips
 *        it. Reached by the PUBLIC one-shot API `gzip::uncompress(data, 0)` /
 *        `deflate::uncompress(data, 0)` and by the generic template with any container.
 *
 *        Driven on a worker thread with a hard deadline so a regression FAILS loudly instead
 *        of hanging the whole suite.
 */
TEST(Compression, EmptyInputUncompressTerminates) {
    std::atomic<bool> finished{false};
    std::atomic<int>  completed{0};

    std::thread worker([&] {
        // Every public shape that routes through the generic template.
        if (qb::gzip::uncompress(nullptr, 0).empty())
            ++completed;
        if (qb::deflate::uncompress(nullptr, 0).empty())
            ++completed;
        std::string out;
        if (qb::compression::uncompress(out, nullptr, 0, 0, 15 + 16) == 0 && out.empty())
            ++completed;
        // The pipe specialisation already had the guard — keep it covered so the pair cannot drift.
        qb::allocator::pipe<char> pipe_out;
        if (qb::compression::uncompress(pipe_out, nullptr, 0, 0, 15 + 16) == 0)
            ++completed;
        finished.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const bool done = finished.load(std::memory_order_acquire);
    if (done)
        worker.join();
    else
        worker.detach(); // it is spinning; let the process reap it at exit

    ASSERT_TRUE(done) << "uncompress(data, 0) never returned — the size == 0 guard is missing";
    EXPECT_EQ(completed.load(), 4) << "every empty-input uncompress shape must yield empty output";
}

// =============================================================================
// ONE-SHOT compress / uncompress: zlib's 32-bit counts (Huly QB-353), an output that throws (Huly QB-354)
// =============================================================================

namespace compression_codec_test {

// The output growth of the chunked helpers into a std::string, recording the widest window asked for.
inline auto
grow_string(std::string &out, std::size_t &widest) {
    return [&out, &widest](std::size_t produced, std::size_t window) {
        widest = (std::max) (widest, window);
        if (out.size() < produced + window)
            out.resize(produced + window);
        return reinterpret_cast<Bytef *>(&out[0] + produced);
    };
}

// An output whose growths throw once `growths` have been granted: a std::bad_alloc in the middle of a one-shot.
struct ThrowingOutput {
    std::string bytes;
    int         growths = 1;

    std::size_t
    size() const noexcept {
        return bytes.size();
    }
    void
    resize(std::size_t n) {
        if (n > bytes.size() && growths-- == 0)
            throw std::bad_alloc();
        bytes.resize(n);
    }
    char &
    operator[](std::size_t i) {
        return bytes[i];
    }
};

// `size` bytes of a 251-byte cycle, written by doubling copies (seconds for gigabytes), ending in a sentinel.
inline std::string
patterned_body(std::size_t size) {
    std::string       body(size, '\0');
    const std::size_t seed = (std::min) (size, std::size_t{251});
    for (std::size_t i = 0; i < seed; ++i)
        body[i] = static_cast<char>('!' + i % 90);
    for (std::size_t filled = seed; filled && filled < size; filled *= 2)
        std::memcpy(body.data() + filled, body.data(), (std::min) (filled, size - filled));
    constexpr char tail[] = "<end of body>";
    if (size >= sizeof tail - 1)
        std::memcpy(body.data() + size - (sizeof tail - 1), tail, sizeof tail - 1);
    return body;
}

} // namespace compression_codec_test

// zlib counts bytes in a 32-bit uInt: the one-shots hand it the input in slices it can count and output windows no
// wider, so a length past 4 GiB is processed whole instead of wrapping to its remainder (Huly QB-353). The always-on
// witness, at the helpers' level with a 7-byte chunk: every window within the bound, the stream intact both ways, the
// bomb and truncation guards still holding. The end-to-end proof past 4 GiB is the opt-in CompressionBeyondUIntMax.
TEST(CompressionOneShot, ZlibIsFedInSlicesAndWindowsItCanCount) {
    namespace detail = qb::compression::detail;
    using compression_codec_test::grow_string;
    static_assert(detail::zlib_max_chunk == 0xFFFFFFFFu, "zlib's uInt is 32 bits wide");

    constexpr std::size_t chunk    = 7;
    const std::string     bodies[] = {
        std::string{}, std::string("q"), qb::io::test::codec_contract::random_bytes(100000, 5) + std::string(100000, 'z')
    };
    for (const std::string &body : bodies) {
        for (const int window_bits : {15, 15 + 16}) {
            std::string       packed;
            std::size_t       widest = 0;
            const std::size_t packed_size =
                detail::deflate_into(body.data(), body.size(), Z_DEFAULT_COMPRESSION, window_bits, grow_string(packed, widest), chunk);
            packed.resize(packed_size);
            EXPECT_LE(widest, chunk) << "a window wider than the chunk";
            ASSERT_GT(packed.size(), 0u) << "even an empty body has a header and a trailer";

            // zlib's whole-buffer one-shot reads the sliced stream back ...
            std::string back;
            EXPECT_NO_THROW((void) qb::compression::uncompress(back, packed.data(), packed.size(), 0, window_bits));
            EXPECT_TRUE(back == body) << body.size() << " bytes came back as " << back.size();

            // ... and so does the sliced inflate, within the same bound.
            std::string       again;
            std::size_t       again_widest = 0;
            const std::size_t again_size =
                detail::inflate_into(packed.data(), packed.size(), 0, window_bits, grow_string(again, again_widest), chunk);
            again.resize(again_size);
            EXPECT_LE(again_widest, chunk) << "a window wider than the chunk";
            EXPECT_TRUE(again == body) << body.size() << " bytes came back as " << again.size();
        }
    }

    // The guards hold through the slices: a bomb past `max`, then a stream cut before its trailer, are refused.
    const std::string zeros(1u << 20, '\0');
    const std::string bomb = qb::gzip::compress(zeros.data(), zeros.size());
    const std::string cut  = bomb.substr(0, bomb.size() - 4);
    std::string       sink;
    std::size_t       widest = 0;
    EXPECT_THROW((void) detail::inflate_into(bomb.data(), bomb.size(), 64u << 10, 15 + 16, grow_string(sink, widest), chunk),
                 std::runtime_error);
    EXPECT_THROW((void) detail::inflate_into(cut.data(), cut.size(), 0, 15 + 16, grow_string(sink, widest), chunk), std::runtime_error);
}

// An output growth that throws -- a std::bad_alloc from resize() -- leaves the one-shot with zlib's state allocated:
// it must still be released. deflateEnd / inflateEnd were skipped and the state leaked, which LeakSanitizer reports
// at exit (Huly QB-354). The pipe one-shots, which APPEND, leave the pipe as they found it when they throw.
TEST(CompressionOneShot, AThrowingOutputReleasesZlibStateAndAPipeIsLeftAsFound) {
    using compression_codec_test::ThrowingOutput;

    // The first window is granted and the second throws: 64 KiB of noise needs two windows of half its size.
    const std::string noise = qb::io::test::codec_contract::random_bytes(64u << 10, 3);
    ThrowingOutput    packed;
    EXPECT_THROW((void) qb::compression::compress(packed, noise.data(), noise.size(), Z_DEFAULT_COMPRESSION, 15 + 16), std::bad_alloc);
    EXPECT_EQ(packed.growths, -1) << "the second growth is the one that threw";

    // Windows of twice the input: a megabyte of zeros needs hundreds of them.
    const std::string zeros(1u << 20, '\0');
    const std::string bomb = qb::gzip::compress(zeros.data(), zeros.size());
    ThrowingOutput    unpacked;
    EXPECT_THROW((void) qb::compression::uncompress(unpacked, bomb.data(), bomb.size(), 0, 15 + 16), std::bad_alloc);
    EXPECT_EQ(unpacked.growths, -1) << "the second growth is the one that threw";

    // The pipe one-shots append after what the pipe holds, and take it back when they throw.
    const std::string         cut = bomb.substr(0, bomb.size() - 4);
    qb::allocator::pipe<char> pipe;
    pipe.put("kept", 4);
    EXPECT_THROW((void) qb::gzip::uncompress(pipe, cut.data(), cut.size()), std::runtime_error);
    EXPECT_EQ(pipe.str(), "kept") << "a truncated stream left " << pipe.size() - 4 << " bytes behind";
    EXPECT_THROW((void) qb::gzip::uncompress(pipe, bomb.data(), bomb.size(), 64u << 10), std::runtime_error);
    EXPECT_EQ(pipe.str(), "kept") << "a refused bomb left " << pipe.size() - 4 << " bytes behind";
    EXPECT_GT(qb::gzip::uncompress(pipe, bomb.data(), bomb.size()), 0u);
    EXPECT_EQ(pipe.size(), 4u + zeros.size()) << "and a call that succeeds appends";
}

// =============================================================================
// LENGTHS PAST 4 GiB (Huly QB-353) -- OPT-IN, never run by default
//
// zlib counts bytes in a 32-bit uInt, and the one-shots narrowed their std::size_t lengths unchecked -- the checks sat
// under `#ifdef DEBUG`, which qb never defines: 4 GiB + 17 bytes of input were compressed as 17, into a valid stream of
// them, and an uncompress whose output window (twice its input) passed 4 GiB counted bytes zlib had never written.
// These cases move real multi-GiB buffers (up to about 10 GB of memory, tens of seconds), so they are DISABLED_ and
// run by name on a host that has the memory:
//
//   qb-io-test-unit-compression-codec --gtest_also_run_disabled_tests --gtest_filter='CompressionBeyondUIntMax.*'
//
// The always-on witness of the same contract is CompressionOneShot.ZlibIsFedInSlicesAndWindowsItCanCount above.
// =============================================================================

TEST(CompressionBeyondUIntMax, DISABLED_OneShotRoundTripsMoreThan4GiB) {
    static_assert(sizeof(std::size_t) >= 8, "a buffer past 4 GiB needs a 64-bit size_t");
    const std::string body = compression_codec_test::patterned_body((std::size_t{1} << 32) + 17);

    std::string packed;
    ASSERT_NO_THROW((void) qb::gzip::compress(packed, body.data(), body.size(), Z_BEST_SPEED));
    packed.shrink_to_fit();

    std::string back;
    back.reserve(body.size() + 2 * packed.size()); // the widest the windows reach: no doubling copies
    ASSERT_NO_THROW((void) qb::gzip::uncompress(back, packed.data(), packed.size()));
    ASSERT_EQ(back.size(), body.size()) << "4 GiB + 17 bytes came back as " << back.size() << ": the length wrapped modulo 2^32";
    EXPECT_TRUE(back == body);
}

TEST(CompressionBeyondUIntMax, DISABLED_UncompressWindowsPast4GiB) {
    static_assert(sizeof(std::size_t) >= 8, "a buffer past 4 GiB needs a 64-bit size_t");
    // Stored (level 0), a stream is longer than its body: past 2 GiB, its first output window -- twice its size -- is
    // past what zlib can count.
    const std::string body = compression_codec_test::patterned_body((std::size_t{1} << 31) + 4096);

    std::string packed;
    ASSERT_NO_THROW((void) qb::gzip::compress(packed, body.data(), body.size(), Z_NO_COMPRESSION));
    packed.shrink_to_fit();
    ASSERT_GT(packed.size(), std::size_t{1} << 31) << "a stored stream is longer than its body";

    std::string back;
    ASSERT_NO_THROW((void) qb::gzip::uncompress(back, packed.data(), packed.size()));
    ASSERT_EQ(back.size(), body.size()) << "2 GiB + 4 KiB came back as " << back.size();
    EXPECT_TRUE(back == body);
}
