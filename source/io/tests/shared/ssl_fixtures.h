/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/ssl_fixtures.h
 * @brief Locate the generated test certificate/key for the qb-io TLS/QUIC suites.
 *
 * Every TLS-bearing qb-io test needs the same two files — `cert.pem` and `key.pem`,
 * the self-signed `CN=localhost` pair CMake's `generate_ssl_certs` target produces and
 * copies into the test binary directory (see system/CMakeLists.txt). The path search was
 * cloned three ways across the suite — the 4-candidate walk in test-ssl-socket.cpp and the
 * bare `__FILE__ + resources/ssl` form in test-session-json.cpp, test-async-io.cpp,
 * test-quic.cpp and test-session-text.cpp — so this header hoists the richest variant into a
 * single source of truth consumed by the `unit/ssl`, `system/tcp/ssl-socket-loopback`,
 * `system/tls`, `system/session` and `system/quic/quic-handshake` targets.
 *
 * `ssl_resource_path()` walks four candidate directories, in priority order:
 *   1. the current working directory                  — `./cert.pem`
 *   2. a `ssl/` subdir of the CWD                      — `./ssl/cert.pem`
 *   3. the committed source-tree resources             — `<repo>/.../system/resources/ssl/`
 *   4. a relative `resources/ssl/`                     — `./resources/ssl/cert.pem`
 * Candidates 1–2 and 4 cover the runtime layouts CMake copies the generated pair into
 * (the certs land next to the test binary); candidate 3 anchors on this header's own
 * `__FILE__` rather than the *test's* `__FILE__`, so it resolves to the one checked-in copy
 * regardless of which subdirectory the including test lives in (the tests sit at varying
 * depths — `unit/ssl/`, `system/tcp/`, `system/quic/` — and a test-relative path would not
 * be portable across them). The first existing candidate wins; if none exists the first
 * (CWD) candidate is returned so callers still get a sensible, reportable path.
 *
 * Per the test-suite spec §7 the harness *ships* the certificate, so a positive TLS test
 * should treat its absence as a hard failure (`ASSERT_TRUE(require_ssl_files())`), not a
 * skip — this header therefore only exposes the predicate + paths and never calls
 * `GTEST_SKIP()` itself, leaving the skip-vs-assert policy to the caller. `require_ssl_files()`
 * is the gate (true iff both files resolve to existing paths); `ssl_files_available()` is its
 * alias for call sites that read more naturally as a query. All helpers are header-only and
 * the TU compiles standalone as a lone include.
 */

#ifndef QB_IO_TESTS_SHARED_SSL_FIXTURES_H
#define QB_IO_TESTS_SHARED_SSL_FIXTURES_H

#include <array>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace qb::io::test {

// ---------------------------------------------------------------------------
// Resolve a file (e.g. "cert.pem" / "key.pem") to the first existing candidate
// among the four search locations. Falls back to the CWD candidate so the
// returned path is always printable in a failure message.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string
ssl_resource_path(const std::string &file_name) {
    // `__FILE__` is THIS header (tests/shared/), so the source-tree copy lives one
    // level up under system/resources/ssl/ — stable no matter where the test sits.
    // lexically_normal() first: `__FILE__` is the include path as WRITTEN, so from a test under
    // e.g. system/tcp/ it carries `..` components that make the purely-lexical parent_path()
    // chain drift off the source tree. See the same note in qbm/http/tests/shared/ssl_test_resource.h.
    const std::filesystem::path here        = std::filesystem::path(__FILE__).lexically_normal().parent_path();
    const std::filesystem::path source_tree = here.parent_path() / "system" / "resources" / "ssl" / file_name;
    const std::filesystem::path cwd         = std::filesystem::current_path();

    const std::array<std::filesystem::path, 4> candidates{
        cwd / file_name,
        cwd / "ssl" / file_name,
        source_tree,
        std::filesystem::path("resources") / "ssl" / file_name,
    };

    for (const auto &candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    return candidates.front().string();
}

// ---------------------------------------------------------------------------
// True iff BOTH the certificate and its private key resolve to existing files.
// The gate a positive TLS test asserts on (the harness ships these files, so a
// failure here is a harness/build problem, not a reason to skip).
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool
require_ssl_files() {
    std::error_code ec;
    const bool      cert_ok = std::filesystem::exists(ssl_resource_path("cert.pem"), ec);
    const bool      key_ok  = std::filesystem::exists(ssl_resource_path("key.pem"), ec);
    return cert_ok && key_ok;
}

// ---------------------------------------------------------------------------
// Query-flavoured alias of `require_ssl_files()` for call sites that read more
// naturally as "are the files available?" — identical semantics.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool
ssl_files_available() {
    return require_ssl_files();
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_SSL_FIXTURES_H
