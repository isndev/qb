/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/compression/codec_contract.h
 * @brief The provider contract every opt-in codec of qb::compression::builtin honours (Huly QB-79).
 *
 * One body of assertions, instantiated per codec by compression-zstd.cpp and compression-brotli.cpp -- each registered
 * `REQUIRES compression <codec>`, so a build without the codec does not build the binary, and a build with it cannot
 * pass vacuously. The contract is the one the zlib pair defines and qbm-http's `Body` relies on:
 *   - the codec is registered for both directions, AFTER gzip and deflate (the server's order of preference);
 *   - an `is_last` call finishes the stream and is repeated until `done`, through any output buffer size;
 *   - calls without `is_last` flush what their input produced, so a stream may be fed in pieces;
 *   - a decompress call with no input still hands back output an earlier call had no room for;
 *   - a truncated stream never reports `done` (and stops making progress); a corrupted one throws;
 *   - `reset()` makes the provider reusable for a second, independent stream.
 */

#ifndef QB_IO_TESTS_UNIT_COMPRESSION_CODEC_CONTRACT_H
#define QB_IO_TESTS_UNIT_COMPRESSION_CODEC_CONTRACT_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/compression.h>

namespace qb::io::test::codec_contract {

using qb::compression::compress_provider;
using qb::compression::decompress_provider;
using qb::compression::operation_hint;

inline std::string
random_bytes(std::size_t n, unsigned seed = 42) {
    std::string  s(n, '\0');
    std::mt19937 rng(seed);
    for (auto &c : s)
        c = static_cast<char>(rng());
    return s;
}

// Compress `input` fed in `in_chunk` pieces (the last one `is_last`), into `out_chunk`-sized output windows.
inline std::string
compress_all(compress_provider &c, const std::string &input, std::size_t in_chunk, std::size_t out_chunk) {
    std::string out;
    std::size_t at   = 0;
    bool        done = false;
    int         idle = 0;
    while (!done) {
        const std::size_t take    = (std::min) (in_chunk, input.size() - at);
        const bool        last    = at + take == input.size();
        std::size_t       used    = 0;
        std::size_t       written = 0;
        std::string       window(out_chunk, '\0');
        written = c.compress(reinterpret_cast<const uint8_t *>(input.data()) + at, take, reinterpret_cast<uint8_t *>(window.data()),
                             window.size(), last ? operation_hint::is_last : operation_hint::has_more, used, done);
        out.append(window.data(), written);
        at += used;
        idle = (written || used) ? 0 : idle + 1;
        if (idle > 3) {
            ADD_FAILURE() << c.algorithm() << ": compress stopped making progress at " << at << " / " << input.size();
            break;
        }
    }
    return out;
}

// Decompress `input` whole, into `out_chunk`-sized output windows; returns what came out, and whether it was done.
inline std::string
decompress_all(decompress_provider &d, const std::string &input, std::size_t out_chunk, bool &done) {
    std::string out;
    std::size_t at   = 0;
    int         idle = 0;
    done             = false;
    while (!done) {
        std::size_t used = 0;
        std::string window(out_chunk, '\0');
        const auto  written = d.decompress(reinterpret_cast<const uint8_t *>(input.data()) + at, input.size() - at,
                                           reinterpret_cast<uint8_t *>(window.data()), window.size(), operation_hint::is_last, used, done);
        out.append(window.data(), written);
        at += used;
        idle = (written || used) ? 0 : idle + 1;
        if (idle > 3)
            break; // no progress: a truncated stream, reported by `done` staying false
    }
    return out;
}

inline void
expect_registered_after_zlib(const std::string &name) {
    namespace builtin = qb::compression::builtin;
    EXPECT_TRUE(builtin::algorithm::supported(name));
    EXPECT_TRUE(builtin::make_compressor(name));
    EXPECT_TRUE(builtin::make_decompressor(name));
    const auto compressors = builtin::get_compress_factories();
    ASSERT_GE(compressors.size(), 3u);
    EXPECT_EQ(compressors[0]->algorithm(), builtin::algorithm::GZIP) << "gzip stays the server's first choice";
    EXPECT_EQ(compressors[1]->algorithm(), builtin::algorithm::DEFLATE);
    EXPECT_TRUE(builtin::get_decompress_factory(name));
}

inline void
expect_round_trips(const std::string &name) {
    const std::string bodies[] = {
        std::string{}, std::string("short and sweet"), random_bytes(1u << 20), std::string(4u << 20, 'a'),
        random_bytes(300000) + std::string(300000, 'z')
    };
    // (input piece, output window): one shot through a roomy window; pieces through a 7-byte window; ...
    const std::pair<std::size_t, std::size_t> shapes[] = {{std::size_t(1) << 30, 1u << 22}, {1000, 7}, {65536, 4096}};
    for (const auto &body : bodies)
        for (const auto &[in_chunk, out_chunk] : shapes) {
            auto       c    = qb::compression::builtin::make_compressor(name);
            auto       d    = qb::compression::builtin::make_decompressor(name);
            const auto pack = compress_all(*c, body, in_chunk, out_chunk);
            bool       done = false;
            const auto back = decompress_all(*d, pack, out_chunk, done);
            EXPECT_TRUE(done) << name << ": " << body.size() << " bytes, pieces of " << in_chunk << ", window " << out_chunk;
            EXPECT_TRUE(back == body) << name << ": " << body.size() << " bytes came back as " << back.size();
        }
}

inline void
expect_truncation_never_done_and_corruption_throws(const std::string &name) {
    const std::string body = random_bytes(200000) + std::string(200000, 'q');
    auto              c    = qb::compression::builtin::make_compressor(name);
    const std::string pack = compress_all(*c, body, std::size_t(1) << 30, 1u << 22);
    ASSERT_GT(pack.size(), 64u);
    for (const std::size_t keep : {pack.size() / 2, pack.size() - 1}) {
        auto       d    = qb::compression::builtin::make_decompressor(name);
        bool       done = false;
        const auto back = decompress_all(*d, pack.substr(0, keep), 4096, done);
        EXPECT_FALSE(done) << name << ": a stream cut at " << keep << " of " << pack.size() << " bytes reported done";
        // Cut in its middle a stream yields less than the body; cut in its trailer (a checksum) it may yield all of it --
        // and still never `done`, which is what tells the caller the body is not to be trusted.
        if (keep == pack.size() / 2)
            EXPECT_LT(back.size(), body.size());
        else
            EXPECT_LE(back.size(), body.size());
    }
    std::string corrupt = pack;
    for (std::size_t i = 8; i < corrupt.size(); i += 7)
        corrupt[i] = static_cast<char>(~corrupt[i]);
    auto d    = qb::compression::builtin::make_decompressor(name);
    bool done = false;
    EXPECT_THROW((void) decompress_all(*d, corrupt, 4096, done), std::runtime_error) << name << ": a corrupted stream was not refused";
}

inline void
expect_reset_reuses(const std::string &name) {
    auto              c = qb::compression::builtin::make_compressor(name);
    auto              d = qb::compression::builtin::make_decompressor(name);
    const std::string one(100000, 'x'), two = random_bytes(50000, 7);
    const auto        first = compress_all(*c, one, std::size_t(1) << 30, 1u << 20);
    c->reset();
    const auto second = compress_all(*c, two, std::size_t(1) << 30, 1u << 20);
    bool       done   = false;
    EXPECT_EQ(decompress_all(*d, first, 1u << 20, done), one);
    EXPECT_TRUE(done);
    d->reset();
    EXPECT_EQ(decompress_all(*d, second, 1u << 20, done), two);
    EXPECT_TRUE(done);
}

} // namespace qb::io::test::codec_contract

#endif // QB_IO_TESTS_UNIT_COMPRESSION_CODEC_CONTRACT_H
