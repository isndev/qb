/**
 * @file unit/crypto/openssl-headers.cpp
 * @brief The OpenSSL headers a qb test compiles are those of the OpenSSL the configure found, and the library it
 *        runs is of their major (Huly QB-1180).
 *
 * `find_package(OpenSSL)` reports a version read from `OPENSSL_INCLUDE_DIR` and links that installation's
 * libraries, but the compiler takes `<openssl/...>` from the first include directory that has it. A prefix-wide
 * one listed earlier -- Homebrew's `/opt/homebrew/include`, which a system nlohmann_json, GoogleTest, zstd or
 * argon2 brings -- also holds the headers of the OpenSSL linked into that prefix: qb compiled openssl@3's 3.5.9
 * headers against the openssl@4 4.1.0 libraries it linked, with every cache entry saying 4, and OpenSSL 4.1's
 * deprecations went unseen. This TU is built like every qb test, so it sees what they see; it also links a decoy
 * include directory the way such a prefix comes in (build-contracts/openssl-decoy, whose opensslv.h is an #error),
 * so a test helper that stops putting the found OpenSSL first fails to build on every lane, one OpenSSL or several.
 *
 * The contracts proven:
 *   - the headers' version is the configure's (`OPENSSL_VERSION`, passed by tests/io/unit/CMakeLists.txt): major and
 *     minor from `OPENSSL_VERSION_NUMBER` (the same bit layout in 1.1 and 3+), and the patch from 3.0 on;
 *   - the library this binary runs (`OpenSSL_version_num()`) is of their major, and no older a minor than them.
 */
#include <gtest/gtest.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

namespace {
constexpr unsigned long headers_major = (OPENSSL_VERSION_NUMBER >> 28) & 0xFUL;
constexpr unsigned long headers_minor = (OPENSSL_VERSION_NUMBER >> 20) & 0xFFUL;
} // namespace

TEST(OpenSslHeaders, AreTheOnesTheConfigureFound) {
    EXPECT_EQ(headers_major, QB_TEST_OPENSSL_FOUND_MAJOR)
        << OPENSSL_VERSION_TEXT " compiled, the configure found OpenSSL '" QB_TEST_OPENSSL_FOUND_VERSION "'";
    EXPECT_EQ(headers_minor, QB_TEST_OPENSSL_FOUND_MINOR)
        << OPENSSL_VERSION_TEXT " compiled, the configure found OpenSSL '" QB_TEST_OPENSSL_FOUND_VERSION "'";
#ifdef OPENSSL_VERSION_PATCH
    EXPECT_EQ(static_cast<unsigned long>(OPENSSL_VERSION_PATCH), QB_TEST_OPENSSL_FOUND_PATCH)
        << OPENSSL_VERSION_TEXT " compiled, the configure found OpenSSL '" QB_TEST_OPENSSL_FOUND_VERSION "'";
#endif
}

TEST(OpenSslHeaders, TheLibraryRunIsOfTheirMajorAndNoOlder) {
    unsigned long const run = OpenSSL_version_num();
    EXPECT_EQ((run >> 28) & 0xFUL, headers_major) << OpenSSL_version(OPENSSL_VERSION) << " runs, " OPENSSL_VERSION_TEXT " compiled";
    EXPECT_GE((run >> 20) & 0xFFUL, headers_minor) << OpenSSL_version(OPENSSL_VERSION) << " runs, " OPENSSL_VERSION_TEXT " compiled";
}
