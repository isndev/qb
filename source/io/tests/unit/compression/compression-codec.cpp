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
#include <thread>
#include <vector>

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
    EXPECT_THROW(qb::gzip::uncompress(out, compressed.c_str(), compressed.size(), static_cast<std::size_t>(1)),
                 std::runtime_error);
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
    EXPECT_NO_THROW(qb::gzip::uncompress(out_ok, compressed.c_str(), compressed.size(),
                                         static_cast<std::size_t>(8 * 1024 * 1024)));
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

TEST(Compression, BuiltinFactoriesAndAlgorithms) {
    namespace builtin = qb::compression::builtin;

    EXPECT_TRUE(builtin::supported());
    EXPECT_TRUE(builtin::algorithm::supported("gzip"));
    EXPECT_TRUE(builtin::algorithm::supported("GZIP"));
    EXPECT_TRUE(builtin::algorithm::supported("DefLate"));
    EXPECT_FALSE(builtin::algorithm::supported("br"));

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
