/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/abi-header-alone.cpp
 * @brief `qb/utility/abi.h` compiles alone without a warning off ELF too, and keeps its link-time
 *        fingerprint retained on ELF (Huly QB-1271).
 *
 * The header is the first and only qb include of this file, so under `QB_TESTS_WERROR` any warning
 * it raises fails this target's build. `QB_ABI_RETAIN` was `__attribute__((retain))` wherever the
 * compiler knew the attribute; GCC honours it on ELF only and warns elsewhere ("'retain' attribute
 * ignored", abi.h's fingerprint array), so every g++ consumer on macOS that builds with -Werror
 * failed to compile. On Mach-O `used` already implies no_dead_strip; on ELF `retain`
 * (SHF_GNU_RETAIN) is what keeps GNU ld's `--gc-sections` from discarding the fingerprint before
 * it resolves the references a mismatched configuration lacks.
 *
 * What is pinned: the macro carries `retain` exactly on ELF (where the compiler knows it) and
 * nothing elsewhere -- the check every CI lane runs, Apple clang on macOS and ELF on Linux; the
 * `-Werror` build only bites under g++ off ELF, which no lane builds -- and the fingerprint array
 * still names its six references. Not covered, and older than this fix: a GCC on ELF whose
 * assembler predates binutils 2.36 also ignores `retain`, with the same warning.
 */

#include <qb/utility/abi.h>

#include <string_view>

#include <gtest/gtest.h>

namespace abi_header_alone_test {
#define ABI_HEADER_ALONE_SPELL_(...) #__VA_ARGS__
#define ABI_HEADER_ALONE_SPELL(...) ABI_HEADER_ALONE_SPELL_(__VA_ARGS__)

constexpr std::string_view retain_spelled = ABI_HEADER_ALONE_SPELL(QB_ABI_RETAIN);

#if defined(__ELF__) && defined(__has_attribute)
#if __has_attribute(retain)
constexpr std::string_view retain_expected = "__attribute__((retain))";
#else
constexpr std::string_view retain_expected = "";
#endif
#else
constexpr std::string_view retain_expected = "";
#endif
} // namespace abi_header_alone_test

/**
 * @test `QB_ABI_RETAIN` is `__attribute__((retain))` on ELF and empty on every other object format.
 * @brief On ELF it is the only thing that keeps the fingerprint through `--gc-sections`; anywhere
 *        else g++ ignores it with a warning (Mach-O, COFF), which `-Werror` turns into a failed build.
 */
TEST(AbiHeaderAlone, RetainsTheFingerprintOnElfAndNowhereElse) {
    EXPECT_EQ(abi_header_alone_test::retain_spelled, abi_header_alone_test::retain_expected);
}

/// @test The fingerprint array still names its six configuration symbols.
TEST(AbiHeaderAlone, TheFingerprintNamesItsReferences) {
    EXPECT_EQ(std::size(qb::detail::abi_fingerprint_actor64), 6u);
}
