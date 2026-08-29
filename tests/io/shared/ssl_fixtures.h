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
 * `__FILE__` rather than the *test's* `__FILE__`, so it resolves regardless of which
 * subdirectory the including test lives in (the tests sit at varying depths — `unit/ssl/`,
 * `system/tcp/`, `system/quic/` — and a test-relative path would not be portable across
 * them). The first existing candidate wins; if none exists the first (CWD) candidate is
 * returned so callers still get a sensible, reportable path.
 *
 * THREE distinct pairs can answer this search, and they are not interchangeable. This
 * paragraph used to call candidate 3 "the one checked-in copy"; there are two checked-in
 * copies, and neither of them is what candidates 1–2 normally find:
 *
 *   - `<build>/bin/tests/{cert,key}.pem` — candidate 1, and what the suite actually runs
 *     against, because `qb_add_test` sets WORKING_DIRECTORY to that directory. It is the
 *     GENERATED pair (`CN=localhost, O=QB Tests, C=US`, with `subjectAltName = DNS:localhost`)
 *     that `generate_ssl_certs` writes on any host with openssl. Exactly one build target
 *     writes it — see `qb_setup_test_resources()` in `qb/cmake/qbFunctions.cmake`, which
 *     stages the committed fallback there ONLY when `generate_ssl_certs` does not exist.
 *   - `qb/resources/ssl/{cert,key}.pem` — the `QB_SSL_RESOURCES` pair
 *     (`qb/cmake/qbDependencies.cmake`). Subject `C=FR, ST=Paris, L=Paris, O=ISNDEV`: no CN
 *     and no subjectAltName, so hostname verification cannot be exercised against it. It is
 *     staged into `<build>/bin/tests/ssl/` (candidate 2), is the openssl-less fallback for
 *     candidate 1, and is the pair the qbm-http suite and the HTTPS/H2/H3 examples resolve
 *     from the source tree.
 *   - `qb/tests/io/system/resources/ssl/{cert,key}.pem` — candidate 3, and the ONLY committed
 *     pair with `CN=localhost`. `system/tcp/ssl-socket-loopback.cpp` addresses it directly
 *     through its own `localhost_fixture()` rather than through this search, precisely
 *     because which pair the search returns depends on the working directory.
 *
 * So: candidate 1 is the contract, candidate 3 is the source-tree safety net that makes
 * `require_ssl_files()` true even in a checkout that was never built.
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
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
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

// ---------------------------------------------------------------------------
// teardown_rendezvous — order the END of a client/server socket pair across
// threads, in either direction.
//
// WHY: the loopback TLS harnesses run the server on a std::thread and the
// client on the test body. On POSIX the teardown order barely matters; on
// Windows it decides the test. Two measured shapes:
//   * the client disconnect()s while the server thread is still inside
//     drive_server_handshake() or a deadline-bounded read — the close lands as
//     WSAECONNRESET on the server side and a de-flaked harness reports it
//     LOUDLY (that is the WINDOWS_EXCLUDE this type exists to retire);
//   * the server's socket destructs while the client is still draining — a
//     Windows RST DISCARDS the peer's undelivered receive buffer, so bytes the
//     server really wrote are never readable.
// Each side therefore signals when it is done with the shared connection, and
// the other side may wait for that signal before closing its end.
//
// The waits are deadline-bounded and return bool rather than asserting: a test
// body that dies on a fatal ASSERT never reaches its signal, so the join
// guard's before_join must ALSO release the rendezvous (both directions), and
// a timed-out wait on the server thread must end the thread, not wedge the
// join. Callers that want loudness assert on the returned bool client-side.
// ---------------------------------------------------------------------------
class teardown_rendezvous {
    std::atomic<bool> _client_done{false};
    std::atomic<bool> _server_done{false};

    [[nodiscard]] static bool
    wait_for(const std::atomic<bool> &flag, std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!flag.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

public:
    void
    client_done() noexcept {
        _client_done.store(true, std::memory_order_release);
    }
    void
    server_done() noexcept {
        _server_done.store(true, std::memory_order_release);
    }
    // Called from a join guard's before_join: whatever side died, nobody waits.
    void
    release_all() noexcept {
        client_done();
        server_done();
    }
    [[nodiscard]] bool
    wait_client(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const noexcept {
        return wait_for(_client_done, timeout);
    }
    [[nodiscard]] bool
    wait_server(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const noexcept {
        return wait_for(_server_done, timeout);
    }
};

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_SSL_FIXTURES_H
