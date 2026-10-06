/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/compression/compression-brotli.cpp
 * @brief The brotli codec against the provider contract of codec_contract.h (Huly QB-79).
 *
 * Built only with QB_WITH_BROTLI (registered `REQUIRES compression brotli`), so it never passes vacuously.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#include "codec_contract.h"

namespace contract = qb::io::test::codec_contract;
namespace builtin  = qb::compression::builtin;

namespace compression_brotli_test {

// The window a brotli stream declares, read from its header (RFC 7932 section 9.1, WBITS, least significant bit first).
inline int
window_bits(const std::string &stream) {
    const unsigned b = static_cast<unsigned char>(stream.at(0)) | (static_cast<unsigned>(static_cast<unsigned char>(stream.at(1))) << 8u);
    if (!(b & 1u))
        return 16;
    if (const unsigned n = (b >> 1u) & 7u)
        return 17 + static_cast<int>(n);
    const unsigned m = (b >> 4u) & 7u;
    return m ? 8 + static_cast<int>(m) : 17;
}

} // namespace compression_brotli_test

TEST(CompressionBrotli, IsRegisteredForBothDirectionsAfterZlib) {
    contract::expect_registered_after_zlib(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, RoundTripsThroughAnyInputPieceAndOutputWindow) {
    contract::expect_round_trips(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, ATruncatedStreamIsNeverDoneAndACorruptOneThrows) {
    contract::expect_truncation_never_done_and_corruption_throws(builtin::algorithm::BROTLI);
}

TEST(CompressionBrotli, ResetMakesTheProvidersReusable) {
    contract::expect_reset_reuses(builtin::algorithm::BROTLI);
}

// On Windows a body given whole in the first call is encoded with the narrowest window that holds it, never wider than
// the configured one: at 16 bits or under brotli takes its light hasher, which made a 4 KiB response 7 times cheaper
// there. Elsewhere the configured window stays, the wide hasher being the faster one on glibc. A body fed in pieces
// keeps the configured window everywhere, its size unknown when encoding starts (Huly QB-93).
TEST(CompressionBrotli, AWholeBodyGetsAWindowThatFitsItOnWindowsOnly) {
    using compression_brotli_test::window_bits;
#if defined(_WIN32)
    constexpr int whole_small_bits = 12;
#else
    constexpr int whole_small_bits = 22;
#endif
    const std::string small_body = contract::random_bytes(3000) + std::string(1096, 'w'); // 4 KiB: 12 bits fit it
    const std::string large_body(5u << 20, 'c');                                          // 5 MiB: 23 bits, capped at 22
    auto              c = builtin::make_compressor(builtin::algorithm::BROTLI);
    auto              d = builtin::make_decompressor(builtin::algorithm::BROTLI);
    struct Case {
        const std::string *body;
        std::size_t        in_chunk;
        int                bits;
    };
    for (const Case &k :
         {Case{&small_body, std::size_t(1) << 30, whole_small_bits}, Case{&large_body, std::size_t(1) << 30, 22},
          Case{&small_body, 1000, 22}}) {
        c->reset();
        d->reset();
        const auto pack = contract::compress_all(*c, *k.body, k.in_chunk, 1u << 22);
        EXPECT_EQ(window_bits(pack), k.bits) << k.body->size() << " bytes in " << k.in_chunk << "-byte pieces";
        bool done = false;
        EXPECT_TRUE(contract::decompress_all(*d, pack, 1u << 20, done) == *k.body) << k.body->size() << " bytes";
        EXPECT_TRUE(done);
    }
}
