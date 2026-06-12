/**
 * @file qb/io/tests/system/test-compression.cpp
 * @brief Unit tests for compression functionality
 *
 * This file contains tests for the compression and decompression functionality
 * in the QB framework, including both gzip and deflate algorithms. It tests
 * single-operation compression/decompression as well as streaming operations.
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
 * @ingroup Tests
 */

#include <gtest/gtest.h>
#include <qb/io/compression.h>
#include <qb/io/crypto.h>
#include <qb/system/allocator/pipe.h>
#include <thread>

TEST(Compression, Gzip) {
    auto compressor   = qb::compression::builtin::make_compressor("gzip");
    auto decompressor = qb::compression::builtin::make_decompressor("gzip");
    auto from         = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> buffer;
    buffer.allocate_back(128000);

    std::size_t i_processed{};
    bool        done{};
    auto        o_processed =
        compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str()),
                             from.size(), reinterpret_cast<uint8_t *>(buffer.begin()),
                             buffer.size(), qb::compression::is_last, i_processed, done);
    buffer.free_back(buffer.size() - o_processed);
    EXPECT_TRUE(done);
    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed = decompressor->decompress(
        reinterpret_cast<uint8_t const *>(buffer.begin()), buffer.size(),
        reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(),
        qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::gzip::uncompress(buffer.begin(), buffer.size()));
}

TEST(Compression, Gzip_Stream) {
    auto compressor   = qb::compression::builtin::make_compressor("gzip");
    auto decompressor = qb::compression::builtin::make_decompressor("gzip");
    auto from         = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> i_buffer, o_buffer;
    i_buffer.allocate_back(128000);

    bool        done{};
    std::size_t o_processed{}, i_processed{};
    while (!done) {
        auto        out = o_buffer.allocate_back(100);
        std::size_t ci_processed{};
        o_processed += compressor->compress(
            reinterpret_cast<uint8_t const *>(from.c_str() + i_processed),
            from.size() - i_processed, reinterpret_cast<uint8_t *>(out),
            o_buffer.size() - o_processed, qb::compression::is_last, ci_processed, done);
        i_processed += ci_processed;
    }

    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed = decompressor->decompress(
        reinterpret_cast<uint8_t const *>(o_buffer.begin()), o_buffer.size(),
        reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(),
        qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::gzip::uncompress(o_buffer.begin(), o_buffer.size()));
}

TEST(Compression, Deflate) {
    auto compressor   = qb::compression::builtin::make_compressor("deflate");
    auto decompressor = qb::compression::builtin::make_decompressor("deflate");
    auto from         = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> buffer;
    buffer.allocate_back(128000);

    std::size_t i_processed{};
    bool        done{};
    auto        o_processed =
        compressor->compress(reinterpret_cast<uint8_t const *>(from.c_str()),
                             from.size(), reinterpret_cast<uint8_t *>(buffer.begin()),
                             buffer.size(), qb::compression::is_last, i_processed, done);
    buffer.free_back(buffer.size() - o_processed);
    EXPECT_TRUE(done);
    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed = decompressor->decompress(
        reinterpret_cast<uint8_t const *>(buffer.begin()), buffer.size(),
        reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(),
        qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::deflate::uncompress(buffer.begin(), buffer.size()));
}

TEST(Compression, Deflate_Stream) {
    auto compressor   = qb::compression::builtin::make_compressor("deflate");
    auto decompressor = qb::compression::builtin::make_decompressor("deflate");
    auto from         = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    qb::allocator::pipe<char> i_buffer, o_buffer;
    i_buffer.allocate_back(128000);

    bool        done{};
    std::size_t o_processed{}, i_processed{};
    while (!done) {
        auto        out = o_buffer.allocate_back(100);
        std::size_t ci_processed{};
        o_processed += compressor->compress(
            reinterpret_cast<uint8_t const *>(from.c_str() + i_processed),
            from.size() - i_processed, reinterpret_cast<uint8_t *>(out),
            o_buffer.size() - o_processed, qb::compression::is_last, ci_processed, done);
        i_processed += ci_processed;
    }

    qb::allocator::pipe<char> buffer2;
    buffer2.allocate_back(128000);
    o_processed = decompressor->decompress(
        reinterpret_cast<uint8_t const *>(o_buffer.begin()), o_buffer.size(),
        reinterpret_cast<uint8_t *>(buffer2.begin()), buffer2.size(),
        qb::compression::is_last, i_processed, done);
    EXPECT_TRUE(done);
    std::string to = buffer2.str();
    EXPECT_EQ(from, to);
    EXPECT_EQ(from, qb::deflate::uncompress(o_buffer.begin(), o_buffer.size()));
}

TEST(Compression, Gzip_All) {
    std::string from = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    std::string compressed_str = qb::gzip::compress(from.c_str(), from.size());
    EXPECT_EQ(from, qb::gzip::uncompress(compressed_str.c_str(), compressed_str.size()));

    qb::allocator::pipe<char> compressed_pipe;
    qb::gzip::to_compress     to_c{from.c_str(), from.size()};
    compressed_pipe << to_c;
    EXPECT_EQ(compressed_str.size(), to_c.size_compressed);
    EXPECT_EQ(compressed_str.size(), compressed_pipe.size());
    EXPECT_EQ(compressed_str,
              std::string(compressed_pipe.begin(), compressed_pipe.size()));

    qb::gzip::to_uncompress   to_uc{compressed_pipe.begin(), compressed_pipe.size()};
    qb::allocator::pipe<char> uncompressed_pipe;
    uncompressed_pipe << to_uc;
    EXPECT_EQ(from.size(), to_uc.size_uncompressed);
    EXPECT_EQ(from.size(), uncompressed_pipe.size());
    EXPECT_EQ(from, std::string(uncompressed_pipe.begin(), uncompressed_pipe.size()));
}

TEST(Compression, Deflate_All) {
    std::string from = qb::crypto::generate_random_string(
        128000, qb::crypto::range_alpha_numeric_special);
    std::string compressed_str = qb::deflate::compress(from.c_str(), from.size());
    EXPECT_EQ(from,
              qb::deflate::uncompress(compressed_str.c_str(), compressed_str.size()));

    qb::allocator::pipe<char> compressed_pipe;
    qb::deflate::to_compress  to_c{from.c_str(), from.size()};
    compressed_pipe << to_c;
    EXPECT_EQ(compressed_str.size(), to_c.size_compressed);
    EXPECT_EQ(compressed_str.size(), compressed_pipe.size());
    EXPECT_EQ(compressed_str,
              std::string(compressed_pipe.begin(), compressed_pipe.size()));

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
    EXPECT_THROW(
        qb::gzip::uncompress(out, compressed.c_str(), compressed.size(),
                             static_cast<std::size_t>(64 * 1024)),
        std::runtime_error);

    // Generous budget → succeeds and round-trips.
    qb::allocator::pipe<char> out_ok;
    EXPECT_NO_THROW(qb::gzip::uncompress(out_ok, compressed.c_str(), compressed.size(),
                                         static_cast<std::size_t>(8 * 1024 * 1024)));
    EXPECT_EQ(out_ok.size(), original.size());
}

// Truncated stream must be rejected (no silent partial output).
TEST(Compression, TruncatedStreamRejected) {
    std::string original = qb::crypto::generate_random_string(
        100000, qb::crypto::range_alpha_numeric_special);
    std::string compressed = qb::gzip::compress(original.c_str(), original.size());
    ASSERT_GT(compressed.size(), 16u);

    // Lop off the tail: the stream can no longer reach Z_STREAM_END.
    std::string truncated = compressed.substr(0, compressed.size() - 8);
    qb::allocator::pipe<char> out;
    EXPECT_THROW(qb::gzip::uncompress(out, truncated.c_str(), truncated.size()),
                 std::runtime_error);
}