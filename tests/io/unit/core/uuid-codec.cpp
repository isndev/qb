/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/uuid-codec.cpp
 * @brief `qb::uuid` text and name codecs -- the stream inserter and to_string, the SHA-1 behind UUIDv5.
 *
 * `qb::uuid` is the vendored stduuid fork (qb/vendor/uuid/include/uuid.h). Two of its codecs are pinned here, both
 * pure arithmetic: NO engine, NO socket, NO event loop.
 *
 *  - The TEXT codec. `operator<<` writes the canonical 36-character form whatever the stream's formatting state, and
 *    leaves that state as it found it except for width(), which pads the whole text as for any formatted inserter
 *    (Huly QB-382). It used to force hex / fill '0' / width 2 per byte and keep the caller's adjustfield and showbase:
 *    on a `std::left` stream the byte 0x04 came out as "40" -- another identifier -- and the fill '0' stayed behind.
 *    The oracle is the identity: the text parses back to the same uuid.
 *  - The NAME codec. UUIDv5 hashes namespace + name with the fork's SHA-1, whose padding wrote the low 32 bits of the
 *    64-bit message bit length and four zero bytes for the high ones (Huly QB-383): from 2^29 bytes hashed (512 MiB,
 *    namespace included) the digest was not SHA-1's. Known answers come from FIPS 180 and RFC 9562 / Python's
 *    hashlib and uuid modules, an implementation independent of this one.
 */

#include <cstdint>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <qb/uuid.h>

namespace uuid_codec_test {

// Bytes chosen so that a per-byte formatting mistake shows: a lone high nibble (0x40), a lone low one (0x04, 0x0a),
// zeros (no showbase prefix is printed for 0), and 0xff.
const uuids::uuid probe{{0x04, 0x0a, 0x00, 0xff, 0x10, 0x20, 0x40, 0x01, 0x80, 0x02, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
constexpr char    canonical[] = "040a00ff-1020-4001-8002-001122334455";

std::string
hex_digest(uuids::detail::sha1 &hasher) {
    uuids::detail::sha1::digest8_t digest;
    hasher.get_digest_bytes(digest);
    std::string hex;
    for (const std::uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 0xF];
    }
    return hex;
}

std::string
sha1_of(std::string const &message) {
    uuids::detail::sha1 hasher;
    hasher.process_bytes(message.data(), message.size());
    return hex_digest(hasher);
}

uuids::uuid
parsed(std::string const &text) {
    const auto id = uuids::uuid::from_string(text);
    return id ? *id : uuids::uuid{};
}

} // namespace uuid_codec_test

// =============================================================================
// TEXT: operator<< and to_string (Huly QB-382)
// =============================================================================

/**
 * @test Whatever adjustfield, basefield, showbase or fill the stream holds, the uuid comes out canonical.
 * @brief The text is the identifier: it must parse back to the same uuid. Before Huly QB-382 `std::left` turned 0x04
 *        into "40", and showbase prefixed the non-zero bytes with "0x".
 */
TEST(UuidText, TheStreamFormatNeverChangesTheIdentifierWritten) {
    using uuid_codec_test::canonical;
    using uuid_codec_test::probe;

    struct Format {
        const char *name;
        void (*apply)(std::ostream &);
    };
    const Format formats[] = {
        {"default", [](std::ostream &) {}},
        {"left", [](std::ostream &os) { os << std::left; }},
        {"right", [](std::ostream &os) { os << std::right; }},
        {"internal", [](std::ostream &os) { os << std::internal; }},
        {"showbase", [](std::ostream &os) { os << std::showbase; }},
        {"internal+showbase", [](std::ostream &os) { os << std::internal << std::showbase; }},
        {"left+fill*", [](std::ostream &os) { os << std::left << std::setfill('*'); }},
        {"dec", [](std::ostream &os) { os << std::dec; }},
        {"oct+showbase", [](std::ostream &os) { os << std::oct << std::showbase; }},
        {"showpos", [](std::ostream &os) { os << std::showpos; }},
    };
    for (const Format &f : formats) {
        std::ostringstream os;
        f.apply(os);
        os << probe;
        EXPECT_EQ(os.str(), canonical) << f.name << ": the stream's formatting state changed the text";
        EXPECT_EQ(uuid_codec_test::parsed(os.str()), probe) << f.name << ": the text names another identifier";
    }

    // Upper-case hexadecimal is the same identifier, and is honoured.
    std::ostringstream upper;
    upper << std::uppercase << probe;
    EXPECT_EQ(upper.str(), "040A00FF-1020-4001-8002-001122334455");
    EXPECT_EQ(uuid_codec_test::parsed(upper.str()), probe);

    // A wide stream writes the same characters.
    std::wostringstream wide;
    wide << std::left << std::showbase << probe;
    EXPECT_EQ(wide.str(), std::wstring(std::begin(canonical), std::end(canonical) - 1));

    // to_string uses a fresh stream: always canonical.
    EXPECT_EQ(uuids::to_string(probe), canonical);
    EXPECT_EQ(uuids::to_string<wchar_t>(probe), std::wstring(std::begin(canonical), std::end(canonical) - 1));
}

/**
 * @test The insertion leaves the stream's flags and fill as it found them, and pads the WHOLE text to width().
 * @brief Before Huly QB-382 the fill stayed '0' (the next padded value printed "007"), and the caller's width was
 *        spent on the first byte. A uuid now behaves as any formatted inserter (a std::string): width() pads the
 *        36 characters with the caller's fill on the caller's side, then width() is 0.
 */
TEST(UuidText, TheInsertionLeavesTheStreamStateAndPadsTheWholeText) {
    using uuid_codec_test::canonical;
    using uuid_codec_test::probe;

    std::ostringstream os;
    os << std::hex << std::showbase;
    const auto flags = os.flags();
    const auto fill  = os.fill();
    os << probe;
    EXPECT_EQ(os.flags(), flags) << "the flags were not restored";
    EXPECT_EQ(os.fill(), fill) << "the fill the inserter used stayed behind";
    EXPECT_EQ(os.width(), 0);
    os << std::dec << std::noshowbase << '|' << std::setw(3) << 7;
    EXPECT_EQ(os.str(), std::string(canonical) + "|  7") << "the next padded value saw the inserter's fill";

    std::ostringstream right;
    right << std::setfill('.') << std::setw(40) << probe << '|';
    EXPECT_EQ(right.str(), std::string("....") + canonical + "|") << "width() must pad the whole identifier";
    EXPECT_EQ(right.width(), 0);
    std::ostringstream left;
    left << std::left << std::setw(38) << probe << '|';
    EXPECT_EQ(left.str(), std::string(canonical) + "  |");
}

// =============================================================================
// NAME: the SHA-1 behind UUIDv5 (Huly QB-383)
// =============================================================================

/**
 * @test The fork's SHA-1 gives FIPS 180's known answers, across one, two and many blocks.
 */
TEST(UuidName, Sha1MatchesTheKnownAnswers) {
    using uuid_codec_test::sha1_of;
    EXPECT_EQ(sha1_of(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(sha1_of("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: the length no longer fits the first block's tail, the padding takes a second one.
    EXPECT_EQ(sha1_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    EXPECT_EQ(sha1_of(std::string(1000000, 'a')), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

/**
 * @test UUIDv5 of a name in a standard namespace is RFC 9562's: version 5, variant 10, the SHA-1 prefix.
 */
TEST(UuidName, Version5MatchesTheReferenceImplementation) {
    uuids::uuid_name_generator dns(uuids::uuid_namespace_dns);
    const uuids::uuid          id = dns("www.example.com");
    EXPECT_EQ(uuids::to_string(id), "2ed6657d-e927-568b-95e1-2665a8aea6a2");
    EXPECT_EQ(id.version(), uuids::uuid_version::name_based_sha1);
    EXPECT_EQ(id.variant(), uuids::uuid_variant::rfc);
    uuids::uuid_name_generator url(uuids::uuid_namespace_url);
    EXPECT_EQ(uuids::to_string(url(std::string("https://cpp.actor/"))), "9e224310-dcbd-5f79-bf35-88576ab1a52c");
    EXPECT_EQ(dns("www.example.com"), id) << "a name generator is a function of the name";
}

#if defined(NDEBUG)
/**
 * @test SHA-1 encodes the message length on 64 bits: from 2^29 bytes the high word of the bit count is no longer 0.
 * @brief Huly QB-383: the padding wrote four zero bytes, then the LOW 32 bits of the bit count. 2^29 bytes are
 *        2^32 bits: the encoded length wrapped to 0 and the digest was another message's. A UUIDv5 reaches it once
 *        namespace (16 bytes) plus name reach 512 MiB. The message is streamed from one zeroed MiB -- no 512 MiB
 *        allocation -- and checked against hashlib on both sides of the boundary.
 *
 *        Optimised builds only: half a gigabyte through the byte-wise compression takes about a second at -O3 and
 *        tens of seconds at -O0 under a sanitizer, which adds nothing to arithmetic on a counter. The release
 *        suites run it; the known answers above run everywhere.
 */
TEST(UuidName, Sha1EncodesTheWholeBitLengthFrom512MiB) {
    const std::vector<std::uint8_t> zeros(std::size_t{1} << 20, 0);

    uuids::detail::sha1 below;
    for (int i = 0; i < 511; ++i)
        below.process_bytes(zeros.data(), zeros.size());
    below.process_bytes(zeros.data(), zeros.size() - 1);
    EXPECT_EQ(uuid_codec_test::hex_digest(below), "7d32aa572655d797397393e83c8204082f7e71e5") << "2^29 - 1 bytes";

    uuids::detail::sha1 at;
    for (int i = 0; i < 512; ++i)
        at.process_bytes(zeros.data(), zeros.size());
    EXPECT_EQ(uuid_codec_test::hex_digest(at), "5b088492c9f4778f409b7ae61477dec124c99033")
        << "2^29 bytes = 2^32 bits: the high 32 bits of the bit length were written as 0";
}
#endif
