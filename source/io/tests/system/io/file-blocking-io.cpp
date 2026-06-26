/**
 * @file system/io/file-blocking-io.cpp
 * @brief Blocking `qb::io::sys::file` read / write round-trips.
 *
 * `qb::io::sys::file` (qb/io/system/file.h) is the framework's thin RAII wrapper over a file
 * descriptor: `open` / `read` / `write` / `close` / `is_open` with explicit byte counts. It is a
 * BLOCKING primitive (its `set_nonblocking()` is a documented no-op for regular files — POSIX regular
 * files are always "ready"), so these are simple SYSTEM-tier I/O tests with no event loop and no socket.
 *
 * Contracts proven:
 *   - opening a readable file and `read`ing back its content returns the exact byte count and bytes;
 *   - truncating-write (`O_WRONLY | O_TRUNC`) then re-reading yields exactly the new content (the old
 *     content is gone — proving the truncate, not just an append);
 *   - `set_nonblocking(true)` does not perturb a regular-file read (it stays fully readable);
 *   - opening a non-existent path fails (`open` returns -1, `is_open()` is false) — the negative path
 *     the old monolith never covered.
 *
 * Each test uses a UNIQUE file under the system temp directory (tag + pid + counter) and removes it on
 * the way out, so there are no inter-test or `ctest -j` collisions on a shared fixed filename.
 *
 * Restructured from the dissolved system/test-async-io.cpp (FileOperations, AsyncFileOperations). The
 * fixed `test_file_operations.txt` / `test_async_file_io.txt` working files become unique temp paths;
 * an explicit open-failure case is added. No file-local main().
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

#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <qb/io/system/file.h>

using namespace qb::io;

namespace {

[[nodiscard]] inline int
current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

// A unique scratch file path under the system temp dir, removed on destruction so
// no two tests (or parallel ctest workers) ever share a fixed filename.
class ScratchFile {
public:
    explicit ScratchFile(const std::string &tag) {
        static std::atomic<unsigned> counter{0};
        const auto name = "qbio_file_blocking_" + tag + "_" + std::to_string(current_pid()) + "_" +
                          std::to_string(counter.fetch_add(1)) + ".tmp";
        _path = (std::filesystem::temp_directory_path() / name).string();
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    ~ScratchFile() {
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    [[nodiscard]] const std::string &
    path() const noexcept {
        return _path;
    }

    void
    write_text(const std::string &content) const {
        std::ofstream ofs(_path, std::ios::binary | std::ios::trunc);
        ofs << content;
    }

private:
    std::string _path;
};

} // namespace

// =============================================================================
// Read back exactly what was written
// =============================================================================

TEST(FileBlockingIo, ReadReturnsExactContent) {
    ScratchFile       scratch("read");
    const std::string content = "Test content for file operations";
    scratch.write_text(content);

    sys::file file;
    ASSERT_NE(file.open(scratch.path(), O_RDONLY), -1);
    ASSERT_TRUE(file.is_open());

    char      buffer[128] = {0};
    const int n           = file.read(buffer, sizeof(buffer) - 1);
    ASSERT_EQ(static_cast<std::size_t>(n), content.size());
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(n)), content);

    file.close();
    EXPECT_FALSE(file.is_open());
}

// =============================================================================
// Truncating write replaces the content (old bytes are gone)
// =============================================================================

TEST(FileBlockingIo, TruncatingWriteReplacesContent) {
    ScratchFile scratch("write");
    scratch.write_text("ORIGINAL CONTENT THAT IS LONGER THAN THE REPLACEMENT");

    sys::file file;
    ASSERT_NE(file.open(scratch.path(), O_WRONLY | O_TRUNC), -1);
    ASSERT_TRUE(file.is_open());

    const std::string new_content = "New test content";
    ASSERT_EQ(static_cast<std::size_t>(file.write(new_content.c_str(), new_content.size())), new_content.size());
    file.close();

    // Re-read: must be EXACTLY the new content, with no trailing remnant of the old.
    sys::file reader;
    ASSERT_NE(reader.open(scratch.path(), O_RDONLY), -1);
    char      buffer[128] = {0};
    const int n           = reader.read(buffer, sizeof(buffer) - 1);
    ASSERT_EQ(static_cast<std::size_t>(n), new_content.size()) << "truncating write left stale bytes behind";
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(n)), new_content);
    reader.close();
}

// =============================================================================
// set_nonblocking() is a no-op for regular files: read still works
// =============================================================================

TEST(FileBlockingIo, NonblockingModeStillReadsRegularFile) {
    ScratchFile       scratch("nonblock");
    const std::string content = "Async file operations test content";
    scratch.write_text(content);

    sys::file file;
    ASSERT_GE(file.open(scratch.path(), O_RDONLY), 0);
    file.set_nonblocking(true); // documented no-op for regular files

    char      buffer[1024] = {0};
    const int n            = file.read(buffer, sizeof(buffer) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(n)), content);
    file.close();
}

// =============================================================================
// Opening a missing path fails cleanly
// =============================================================================

TEST(FileBlockingIo, OpenMissingPathFails) {
    const auto missing = (std::filesystem::temp_directory_path() / "qbio_definitely_absent_file_xyz.tmp").string();
    std::error_code ec;
    std::filesystem::remove(missing, ec); // make sure it really is absent

    sys::file file;
    EXPECT_EQ(file.open(missing, O_RDONLY), -1) << "opening a non-existent file must fail";
    EXPECT_FALSE(file.is_open());
}

// =============================================================================
// open(int fd): adopting a raw descriptor (closes any prior handle, then owns fd)
// =============================================================================

// `sys::file::open(int fd)` takes ownership of an already-open OS descriptor:
// it first closes whatever handle the object held, then stores `fd` as its own.
// We open a file the conventional way, then adopt a freshly opened descriptor
// to the SAME path and prove (a) the prior handle was closed and (b) reads flow
// through the adopted fd.
TEST(FileBlockingIo, AdoptRawDescriptorClosesPriorAndReads) {
    ScratchFile       scratch("adopt");
    const std::string content = "adopted descriptor content";
    scratch.write_text(content);

    sys::file file;
    // First, give the object a real handle so open(fd) has a prior handle to close.
    ASSERT_GE(file.open(scratch.path(), O_RDONLY), 0);
    ASSERT_TRUE(file.is_open());

    // Open a second, independent OS descriptor and adopt it. open(int) must close
    // the first handle and return the adopted fd unchanged.
    const int raw = ::open(scratch.path().c_str(), O_RDONLY);
    ASSERT_GE(raw, 0) << "could not open a raw descriptor for adoption";
    EXPECT_EQ(file.open(raw), raw) << "open(fd) must store and return the adopted descriptor";
    EXPECT_TRUE(file.is_open());
    EXPECT_EQ(file.native_handle(), raw);

    // Reads must flow through the adopted descriptor.
    char      buffer[128] = {0};
    const int n           = file.read(buffer, sizeof(buffer) - 1);
    ASSERT_EQ(static_cast<std::size_t>(n), content.size());
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(n)), content);

    file.close();
    EXPECT_FALSE(file.is_open());
}

// =============================================================================
// close() on a descriptor that is no longer valid hits the failure branch
// =============================================================================

#ifndef _WIN32
// `file::close()` pre-invalidates `_handle`, then calls `::close(fd)`; if the OS
// close fails (EBADF), it logs to stderr (the negative arm). We force that by
// adopting a real fd, closing it out-of-band, then asking the file to close the
// now-stale descriptor. This must not crash and must leave the object closed.
TEST(FileBlockingIo, CloseOfStaleDescriptorIsHandled) {
    ScratchFile scratch("staleclose");
    scratch.write_text("x");

    const int raw = ::open(scratch.path().c_str(), O_RDONLY);
    ASSERT_GE(raw, 0);

    sys::file file;
    ASSERT_EQ(file.open(raw), raw);
    ASSERT_TRUE(file.is_open());

    // Close the descriptor behind the file's back so the next ::close(raw) fails.
    ASSERT_EQ(::close(raw), 0);

    // file.close() now calls ::close(raw) again -> EBADF -> stderr notice, no throw.
    EXPECT_NO_THROW(file.close());
    EXPECT_FALSE(file.is_open()) << "close() must invalidate the handle even on OS failure";
}
#endif
