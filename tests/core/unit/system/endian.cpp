/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/system/endian.cpp
 * @brief `qb::byteswap` (`qb/utility/compat.h`) and the `qb::endian` conversions (`qb/system/endian.h`), untested
 *        until 3.3.
 *
 * `qb::byteswap` is `std::byteswap` where the library has it (C++23) and a portable loop otherwise (C++20). The
 * two paths must agree on every type the contract admits -- and the contract admits `bool`, which is integral:
 *   - ONE BYTE (Huly QB-358): `bool`, `char`, `int8_t`, `uint8_t` and a one-byte enum swap to themselves. The C++20
 *     fallback took `std::make_unsigned_t<T>` first, ill-formed for `bool`, so `qb::byteswap(true)` and
 *     `qb::endian::byteswap(true)` did not COMPILE under C++20 while they did under C++23 -- the witness is this
 *     file building at all under the `dev` (C++20) preset, and the `static_assert`s below;
 *   - WIDER: the byte order of 16/32/64-bit values, signed and unsigned, and of a wide enum, is reversed exactly;
 *   - FLOATING POINT: `qb::endian::byteswap` reverses the object representation of a `float` / `double`, and twice
 *     is the identity;
 *   - CONVERSIONS: `to_big_endian` / `from_big_endian` / `to_little_endian` / `from_little_endian` swap on the
 *     opposite-order host only, and each pair round-trips.
 */

#include <gtest/gtest.h>
#include <qb/system/endian.h>
#include <qb/utility/compat.h>
#include <cstdint>
#include <cstring>

namespace endian_test {

enum class Small : std::uint8_t { a = 0x5a };
enum class Wide : std::uint32_t { a = 0x11223344u };

// The C++20 path is the one that failed for bool: these must hold at compile time on both paths.
static_assert(qb::byteswap(true) == true);
static_assert(qb::byteswap(false) == false);
static_assert(qb::byteswap(static_cast<char>(0x7f)) == static_cast<char>(0x7f));
static_assert(qb::byteswap(static_cast<std::int8_t>(-3)) == static_cast<std::int8_t>(-3));
static_assert(qb::byteswap(static_cast<std::uint8_t>(0xa5)) == static_cast<std::uint8_t>(0xa5));
static_assert(qb::byteswap(Small::a) == Small::a);
static_assert(qb::byteswap(static_cast<std::uint16_t>(0x1234u)) == 0x3412u);
static_assert(qb::byteswap(static_cast<std::uint32_t>(0x11223344u)) == 0x44332211u);
static_assert(qb::byteswap(static_cast<std::uint64_t>(0x0102030405060708ull)) == 0x0807060504030201ull);
static_assert(qb::byteswap(Wide::a) == static_cast<Wide>(0x44332211u));
static_assert(qb::endian::byteswap(true) == true);

TEST(Endian, OneByteTypesSwapToThemselves) {
    volatile bool t = true; // a runtime value too: the non-constexpr instantiation must compile and hold
    EXPECT_EQ(qb::byteswap(static_cast<bool>(t)), true);
    EXPECT_EQ(qb::endian::byteswap(static_cast<bool>(t)), true);
    EXPECT_EQ(qb::byteswap(static_cast<std::int8_t>(-128)), static_cast<std::int8_t>(-128));
    EXPECT_EQ(qb::byteswap(Small::a), Small::a);
}

TEST(Endian, WiderIntegersReverseTheirBytes) {
    EXPECT_EQ(qb::byteswap(static_cast<std::int16_t>(0x0102)), static_cast<std::int16_t>(0x0201));
    EXPECT_EQ(qb::byteswap(static_cast<std::int32_t>(-2)), static_cast<std::int32_t>(0xfeffffff));
    EXPECT_EQ(qb::byteswap(static_cast<std::uint64_t>(0xff00000000000000ull)), 0xffull);
    // twice is the identity, signed included
    EXPECT_EQ(qb::byteswap(qb::byteswap(static_cast<std::int64_t>(-123456789012345ll))), -123456789012345ll);
}

TEST(Endian, FloatingPointReversesItsRepresentation) {
    const double  d       = 1.5;
    const double  swapped = qb::endian::byteswap(d);
    unsigned char a[sizeof d], b[sizeof d];
    std::memcpy(a, &d, sizeof d);
    std::memcpy(b, &swapped, sizeof d);
    for (std::size_t i = 0; i < sizeof d; ++i)
        EXPECT_EQ(a[i], b[sizeof d - 1 - i]) << "byte " << i;
    EXPECT_EQ(qb::endian::byteswap(swapped), d);
    EXPECT_EQ(qb::endian::byteswap(qb::endian::byteswap(2.25f)), 2.25f);
}

TEST(Endian, ConversionsSwapOnTheOppositeOrderOnly) {
    const std::uint32_t v = 0x11223344u;
    if constexpr (qb::endian::is_little_endian()) {
        EXPECT_EQ(qb::endian::to_big_endian(v), 0x44332211u);
        EXPECT_EQ(qb::endian::to_little_endian(v), v);
    } else {
        EXPECT_EQ(qb::endian::to_big_endian(v), v);
        EXPECT_EQ(qb::endian::to_little_endian(v), 0x44332211u);
    }
    EXPECT_EQ(qb::endian::from_big_endian(qb::endian::to_big_endian(v)), v);
    EXPECT_EQ(qb::endian::from_little_endian(qb::endian::to_little_endian(v)), v);
    EXPECT_EQ(qb::endian::to_big_endian(true), true);
}

} // namespace endian_test
