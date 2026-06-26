/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/file/self-locate.cpp
 * @brief `qb::io::sys::self_path` / `self_dir` / `resolve_resource` — executable-relative resolution.
 *
 * These helpers (qb/io/system/file.h) let a binary find assets relative to *itself* rather than the
 * working directory: `self_path()` queries the OS for the running executable's absolute path
 * (`/proc/self/exe`, `_NSGetExecutablePath`, `GetModuleFileNameW`), `self_dir()` is its parent, and
 * `resolve_resource()` resolves a path CWD-first then exe-dir-fallback (absolute paths pass
 * through untouched). Pure local-filesystem queries — no event loop, no socket — so this is `unit`.
 *
 * The contracts proven:
 *   - self_path() is a non-empty, absolute, existing regular file whose stem carries the stable
 *     "test" marker (asserted on the marker, NOT a hard-coded binary name, so it survives the
 *     qb-io-test-<tier>-<name> renames);
 *   - self_dir() equals self_path().parent_path() and is a directory;
 *   - resolve_resource() leaves an absolute path untouched;
 *   - resolve_resource() prefers a CWD-relative match (historical behaviour);
 *   - resolve_resource() falls back to the executable directory when the CWD misses
 *     (this is what makes a binary self-contained);
 *   - resolve_resource() returns the input unchanged when nothing exists anywhere.
 *
 * Restructured from the dissolved system/test-file-operations.cpp SelfLocate suite (free TESTs).
 * Per-file main() dropped for the shared gtest main.
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

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/system/file.h>

// =============================================================================
// self_path / self_dir
// =============================================================================

/**
 * @test self_path() points at this (test) executable.
 * @brief Non-empty, absolute, existing regular file. The filename is asserted via the stable
 *        "test" marker rather than a specific name, so the assertion survives target renames
 *        (the binary is qb-io-test-<tier>-<name>).
 */
TEST(SelfLocate, SelfPathPointsAtThisExecutable) {
    const std::filesystem::path exe = qb::io::sys::self_path();
    ASSERT_FALSE(exe.empty()) << "self_path() returned empty";
    EXPECT_TRUE(exe.is_absolute());

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(exe, ec)) << exe.string();
    EXPECT_TRUE(std::filesystem::is_regular_file(exe, ec)) << exe.string();

    const std::string stem = exe.stem().string();
    EXPECT_FALSE(stem.empty()) << exe.string();
    EXPECT_NE(stem.find("test"), std::string::npos)
        << "self_path() must point at this test binary: " << exe.string();
}

/**
 * @test self_dir() is the parent directory of self_path().
 */
TEST(SelfLocate, SelfDirIsTheParentOfSelfPath) {
    const std::filesystem::path dir = qb::io::sys::self_dir();
    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(dir, qb::io::sys::self_path().parent_path());

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(dir, ec)) << dir.string();
}

// =============================================================================
// resolve_resource
// =============================================================================

/**
 * @test An absolute path is returned unchanged.
 */
TEST(SelfLocate, ResolveResourceLeavesAbsolutePathsUntouched) {
    const std::filesystem::path abs = qb::io::sys::self_path(); // a known absolute path
    EXPECT_EQ(qb::io::sys::resolve_resource(abs), abs);
}

/**
 * @test A path that exists relative to the CWD resolves to that CWD copy.
 * @brief Historical behaviour: the working directory is consulted before the executable directory.
 */
TEST(SelfLocate, ResolveResourcePrefersTheWorkingDirectory) {
    const std::filesystem::path rel = "qb_selflocate_cwd_probe.tmp";
    std::ofstream(rel) << "x";

    EXPECT_EQ(qb::io::sys::resolve_resource(rel), rel);

    std::error_code ec;
    std::filesystem::remove(rel, ec);
}

/**
 * @test A path present next to the executable but absent from the CWD resolves to the exe-dir copy.
 * @brief This is what makes a binary self-contained. The CWD is moved to a directory that does NOT
 *        contain the probe so the working-directory lookup misses and the exe-dir fallback fires.
 *        The CWD is restored before any assertion so a failure cannot leave the runner stranded.
 */
TEST(SelfLocate, ResolveResourceFallsBackToExecutableDirectory) {
    const std::filesystem::path name   = "qb_selflocate_exedir_probe.tmp";
    const std::filesystem::path staged = qb::io::sys::self_dir() / name;
    std::ofstream(staged) << "x";

    const std::filesystem::path prev = std::filesystem::current_path();
    const std::filesystem::path tmp  = std::filesystem::temp_directory_path();
    std::error_code             ec;
    std::filesystem::current_path(tmp, ec);

    const std::filesystem::path resolved = qb::io::sys::resolve_resource(name);

    std::filesystem::current_path(prev, ec); // restore before asserting
    std::filesystem::remove(staged, ec);

    EXPECT_TRUE(resolved.is_absolute()) << resolved.string();
    EXPECT_EQ(resolved, (qb::io::sys::self_dir() / name).lexically_normal());
}

/**
 * @test A path that exists nowhere is returned unchanged so diagnostics report what was requested.
 */
TEST(SelfLocate, ResolveResourceReturnsInputWhenNothingExists) {
    const std::filesystem::path missing = "qb_selflocate_does_not_exist_anywhere.tmp";
    EXPECT_EQ(qb::io::sys::resolve_resource(missing), missing);
}
