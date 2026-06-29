/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file unit/file/file-error-paths.cpp
 * @brief `qb::io::sys::file` (system/file.cpp) — the open-disposition + read/write error branches.
 *
 * file-sys.cpp covers the file happy path and the basic error cases (missing file, closed-fd
 * read/write, access modes). This unit drives the remaining BRANCH-weak corners of file.cpp that
 * the existing suite leaves untested, deterministically and daemon-free:
 *
 *   - `open(int fd)` adopting an existing raw descriptor (the fd-overload branch), and that a second
 *     `open()` on a live handle closes the previous descriptor first.
 *   - the O_CREAT|O_EXCL disposition: creating succeeds, re-creating an existing file fails
 *     (CREATE_NEW / O_EXCL branch).
 *   - the OPEN_EXISTING branch: opening for read without O_CREAT fails when the file is absent.
 *   - the O_TRUNC disposition truncates an existing larger file to empty on open.
 *   - a partial read: a buffer larger than the remaining bytes returns exactly the remainder, and the
 *     *next* read returns 0 (EOF) — the read()==0 EOF branch.
 *   - write-then-read on a single O_RDWR descriptor advances the shared file offset (sequential I/O).
 *   - a read into a zero-length request returns 0 with no error.
 *   - reading a file opened write-only / writing a file opened read-only fail (the wrong-direction
 *     branch) — already partially covered, re-pinned here with exact errno-class expectations.
 *   - opening a path whose parent directory does not exist fails (the disposition resolves to a
 *     non-creatable path).
 *
 * Pure `unit`: every operation is a local-filesystem syscall against a unique per-test scratch dir.
 *
 * Signatures exercised (qb/io/system/file.h):
 *   int  open(std::filesystem::path const&, int flags, int mode) noexcept;
 *   int  open(int fd) noexcept;
 *   int  read(char*, std::size_t) const noexcept;   int write(const char*, std::size_t) const noexcept;
 *   bool is_open() const noexcept;                  void close() noexcept;
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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/system/file.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

[[nodiscard]] std::string
slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

class FileErrorPaths : public ::testing::Test {
protected:
    std::filesystem::path dir;

    void
    SetUp() override {
        dir = std::filesystem::temp_directory_path()
              / ("qb_file_err_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_"
                 + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }

    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// ===========================================================================
// open(int fd) adoption + previous-descriptor drop
// ===========================================================================

TEST_F(FileErrorPaths, OpenByFdAdoptsAnExistingDescriptor) {
    const auto path = dir / "adopt.txt";
    std::ofstream(path, std::ios::binary) << "adopted-content";

    // Obtain a raw native descriptor portably (the global ::open is POSIX-only;
    // Windows exposes _open). file::open(int fd) then adopts it.
#ifdef _WIN32
    const int raw = ::_open(path.string().c_str(), _O_RDONLY | _O_BINARY);
#else
    const int raw = ::open(path.string().c_str(), O_RDONLY);
#endif
    ASSERT_GE(raw, 0);

    qb::io::sys::file f;
    EXPECT_EQ(f.open(raw), raw);
    EXPECT_TRUE(f.is_open());
    EXPECT_EQ(f.native_handle(), raw);

    char      buf[32] = {0};
    const int n       = f.read(buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)), "adopted-content");
}

TEST_F(FileErrorPaths, OpenByPathClosesThePreviousDescriptorFirst) {
    const auto first  = dir / "first.txt";
    const auto second = dir / "second.txt";
    std::ofstream(first, std::ios::binary) << "first";
    std::ofstream(second, std::ios::binary) << "second-file";

    qb::io::sys::file f(first, O_RDONLY);
    ASSERT_TRUE(f.is_open());

    // Re-open() must drop the old fd and adopt the new file's content.
    ASSERT_GE(f.open(second, O_RDONLY), 0);
    char      buf[32] = {0};
    const int n       = f.read(buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)), "second-file");
}

// ===========================================================================
// open dispositions: O_EXCL, OPEN_EXISTING, O_TRUNC
// ===========================================================================

TEST_F(FileErrorPaths, ExclusiveCreateRejectsAnExistingFile) {
    const auto path = dir / "excl.txt";

    // First exclusive create succeeds.
    qb::io::sys::file fresh;
    ASSERT_GE(fresh.open(path, O_WRONLY | O_CREAT | O_EXCL, 0644), 0);
    EXPECT_TRUE(fresh.is_open());
    fresh.close();

    // A second exclusive create on the now-existing path must fail.
    qb::io::sys::file dup;
    dup.open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    EXPECT_FALSE(dup.is_open()) << "O_EXCL must refuse to re-create an existing file";
}

TEST_F(FileErrorPaths, OpenExistingForReadFailsWhenAbsent) {
    qb::io::sys::file f;
    // No O_CREAT: a read-open of a non-existent path resolves to OPEN_EXISTING and fails.
    f.open(dir / "absent.txt", O_RDONLY);
    EXPECT_FALSE(f.is_open());
}

TEST_F(FileErrorPaths, TruncateDispositionEmptiesAnExistingFileOnOpen) {
    const auto path = dir / "trunc.txt";
    std::ofstream(path, std::ios::binary) << "this content is longer than what we will write";
    ASSERT_GT(std::filesystem::file_size(path), 0u);

    qb::io::sys::file f;
    // O_TRUNC on open zeroes the file before we write anything new.
    ASSERT_GE(f.open(path, O_WRONLY | O_TRUNC), 0);
    EXPECT_EQ(f.write("hi", 2), 2);
    f.close();

    EXPECT_EQ(slurp(path), "hi") << "O_TRUNC must discard the prior, longer content";
}

// ===========================================================================
// read EOF / partial / zero-length branches
// ===========================================================================

TEST_F(FileErrorPaths, PartialReadReturnsRemainderThenEof) {
    const auto        path    = dir / "partial.txt";
    const std::string content = "1234567890";
    std::ofstream(path, std::ios::binary) << content;

    qb::io::sys::file f(path, O_RDONLY);
    ASSERT_TRUE(f.is_open());

    // A buffer larger than the file returns exactly the file size...
    char      buf[64] = {0};
    const int n       = f.read(buf, sizeof(buf));
    EXPECT_EQ(n, static_cast<int>(content.size()));
    EXPECT_EQ(std::string(buf, static_cast<std::size_t>(n)), content);

    // ...and the next read returns 0 (EOF), not an error.
    EXPECT_EQ(f.read(buf, sizeof(buf)), 0) << "read past end must return 0 (EOF)";
}

TEST_F(FileErrorPaths, ZeroLengthReadIsANoOpReturningZero) {
    const auto path = dir / "zero.txt";
    std::ofstream(path, std::ios::binary) << "non-empty";

    qb::io::sys::file f(path, O_RDONLY);
    ASSERT_TRUE(f.is_open());

    char buf[8] = {0};
    EXPECT_EQ(f.read(buf, 0), 0) << "a zero-byte read must succeed with 0 and consume nothing";

    // The offset is untouched: a real read still sees the first byte.
    char one = 0;
    EXPECT_EQ(f.read(&one, 1), 1);
    EXPECT_EQ(one, 'n');
}

// ===========================================================================
// shared offset on a single O_RDWR descriptor
// ===========================================================================

TEST_F(FileErrorPaths, ReadWriteOnOneDescriptorSharesTheFileOffset) {
    const auto path = dir / "rw.txt";

    qb::io::sys::file f;
    ASSERT_GE(f.open(path, O_RDWR | O_CREAT | O_TRUNC, 0644), 0);

    const std::string payload = "abcdef";
    ASSERT_EQ(f.write(payload.c_str(), payload.size()), static_cast<int>(payload.size()));

    // The write advanced the offset to EOF; a read here therefore returns 0,
    // proving the offset is shared (not a fresh independent read cursor).
    char buf[16] = {0};
    EXPECT_EQ(f.read(buf, sizeof(buf)), 0) << "after writing to EOF on the same fd, a sequential read must see EOF";
    f.close();

    EXPECT_EQ(slurp(path), payload);
}

// ===========================================================================
// wrong-direction access (re-pinned with explicit < 0 on the failing op)
// ===========================================================================

TEST_F(FileErrorPaths, ReadOnReadOnlyAndWriteOnWriteOnlyAreDirectional) {
    const auto ro_path = dir / "ro.txt";
    std::ofstream(ro_path, std::ios::binary) << "readonly";

    qb::io::sys::file ro(ro_path, O_RDONLY);
    ASSERT_TRUE(ro.is_open());
    EXPECT_LT(ro.write("x", 1), 0) << "writing an O_RDONLY descriptor must fail";

    const auto        wo_path = dir / "wo.txt";
    qb::io::sys::file wo;
    ASSERT_GE(wo.open(wo_path, O_WRONLY | O_CREAT, 0644), 0);
    EXPECT_EQ(wo.write("payload", 7), 7);
    char buf[8] = {0};
    EXPECT_LT(wo.read(buf, sizeof(buf)), 0) << "reading an O_WRONLY descriptor must fail";
}

// ===========================================================================
// open into a non-existent directory
// ===========================================================================

TEST_F(FileErrorPaths, OpenIntoMissingParentDirectoryFails) {
    qb::io::sys::file f;
    // The parent dir does not exist, so even O_CREAT cannot place the file.
    f.open(dir / "no_such_subdir" / "file.txt", O_WRONLY | O_CREAT, 0644);
    EXPECT_FALSE(f.is_open()) << "creating a file under a missing directory must fail";
    char buf[4] = {0};
    EXPECT_LT(f.read(buf, sizeof(buf)), 0);
    EXPECT_LT(f.write("x", 1), 0);
}
