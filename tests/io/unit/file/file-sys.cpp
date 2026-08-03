/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/file/file-sys.cpp
 * @brief `qb::io::sys::file` — the synchronous native-descriptor wrapper, end to end.
 *
 * `sys::file` (qb/io/system/file.h) is a thin, platform-independent RAII handle around a native
 * file descriptor: open/read/write/close, the three constructor overloads (default, by-path,
 * by-fd), move-only descriptor ownership (copy is `= delete`), and a closing destructor. Nothing
 * here touches the event loop, a socket, or a daemon — every operation is a deterministic local
 * filesystem syscall — so this is a pure `unit` test.
 *
 * Contracts proven:
 *   - construction: default is closed; by-path opens (or stays closed on a bad path); by-fd adopts.
 *   - read/write round-trips against ground-truth on disk (std::ifstream) and back.
 *   - access modes: O_RDONLY rejects write, O_WRONLY rejects read, O_APPEND appends to existing.
 *   - move semantics transfer descriptor ownership and leave the source closed (no double-close).
 *   - edge cases: empty file → 0-byte read (EOF); 1-byte buffer; zero-size read/write; O_TRUNC.
 *   - error paths: open of a missing file fails; read/write on a closed fd return < 0.
 *
 * Restructured from the dissolved system/test-file-operations.cpp
 * (BasicFileOperations, ConstructorOverloads, FileMoveTransfersDescriptorOwnership, ErrorHandling,
 * FileAccessModes, FileEdgeCases). The flaky `ConcurrentOperations` writer/reader-with-sleeps test
 * is replaced by a deterministic, synchronisation-primitive-free reader-after-writer prefix-match
 * (WriterThenReaderPrefixMatches) that asserts the bytes read are an exact prefix of the bytes
 * written, not merely `total_read > 0`. The per-file main() is dropped in favour of the shared
 * gtest main. `CharPipeTypedPutAndStreamOutput` is intentionally NOT here — it migrates to the
 * pipe/json unit per the spec.
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
#include <type_traits>

#include <gtest/gtest.h>
#include <qb/io/system/file.h>

namespace {

/// Read an entire file from disk as ground truth, independently of `sys::file`.
[[nodiscard]] std::string
slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

// =============================================================================
// TEST FIXTURE — a unique per-test scratch directory on the local filesystem.
// =============================================================================

class FileSysTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir;
    std::filesystem::path test_file;
    const std::string     test_content = "Hello, QB File System!";

    void
    SetUp() override {
        // A per-test directory under the OS temp area keeps parallel test processes isolated
        // (no shared "./test_files" collision) and survives any working-directory churn.
        test_dir = std::filesystem::temp_directory_path()
                   / ("qb_file_sys_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_"
                      + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(test_dir);
        std::filesystem::create_directories(test_dir);

        test_file = test_dir / "test.txt";
        std::ofstream(test_file, std::ios::binary) << test_content;
    }

    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }
};

// =============================================================================
// CONSTRUCTION / OWNERSHIP MODEL
// =============================================================================

/**
 * @test sys::file is move-only — copy is deleted, move is available.
 * @brief Compile-time proof of the owning-descriptor value semantics documented in file.h.
 */
TEST(FileSys, IsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<qb::io::sys::file>);
    static_assert(!std::is_copy_assignable_v<qb::io::sys::file>);
    static_assert(std::is_move_constructible_v<qb::io::sys::file>);
    static_assert(std::is_move_assignable_v<qb::io::sys::file>);
}

/**
 * @test Default constructor yields a closed file.
 */
TEST(FileSys, DefaultConstructedIsClosed) {
    qb::io::sys::file f;
    EXPECT_FALSE(f.is_open());
    EXPECT_EQ(f.native_handle(), -1);
}

/**
 * @test The three constructor overloads — default (closed), by-path (open), by-fd (adopt).
 */
TEST_F(FileSysTest, ConstructorOverloads) {
    qb::io::sys::file by_default;
    EXPECT_FALSE(by_default.is_open());

    qb::io::sys::file by_path(test_file, O_RDONLY);
    EXPECT_TRUE(by_path.is_open());

    const int raw = ::open(test_file.string().c_str(), O_RDONLY);
    ASSERT_GE(raw, 0);
    qb::io::sys::file by_fd(raw);
    EXPECT_TRUE(by_fd.is_open());
    EXPECT_EQ(by_fd.native_handle(), raw);
}

// =============================================================================
// READ / WRITE ROUND-TRIP
// =============================================================================

/**
 * @test Open → read → close, then open-for-write → write → close, verified against disk.
 * @brief Salvaged from BasicFileOperations; reads the exact content and round-trips a write.
 */
TEST_F(FileSysTest, OpenReadWriteCloseRoundTrip) {
    qb::io::sys::file reader;
    ASSERT_GE(reader.open(test_file, O_RDONLY), 0);
    ASSERT_TRUE(reader.is_open());

    char      buffer[128] = {0};
    const int bytes_read  = reader.read(buffer, sizeof(buffer) - 1);
    EXPECT_EQ(bytes_read, static_cast<int>(test_content.size()));
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(bytes_read)), test_content);

    reader.close();
    EXPECT_FALSE(reader.is_open());

    const std::filesystem::path out = test_dir / "write_test.txt";
    qb::io::sys::file           writer;
    ASSERT_GE(writer.open(out, O_WRONLY | O_CREAT, 0644), 0);
    ASSERT_TRUE(writer.is_open());

    const std::string payload = "Writing test data";
    EXPECT_EQ(writer.write(payload.c_str(), payload.size()), static_cast<int>(payload.size()));
    writer.close();

    EXPECT_EQ(slurp(out), payload);
}

// =============================================================================
// MOVE SEMANTICS
// =============================================================================

/**
 * @test Move construction and move assignment transfer descriptor ownership.
 * @brief The source is left closed (no double-close), the destination keeps the same native fd,
 *        and the moved-into handle is still readable.
 */
TEST_F(FileSysTest, MoveTransfersDescriptorOwnership) {
    qb::io::sys::file source(test_file, O_RDONLY);
    ASSERT_TRUE(source.is_open());
    const int original_handle = source.native_handle();

    qb::io::sys::file moved(std::move(source));
    EXPECT_TRUE(moved.is_open());
    EXPECT_FALSE(source.is_open());
    EXPECT_EQ(moved.native_handle(), original_handle);

    qb::io::sys::file assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.is_open());
    EXPECT_FALSE(moved.is_open());
    EXPECT_EQ(assigned.native_handle(), original_handle);

    char buffer[8] = {0};
    EXPECT_GT(assigned.read(buffer, sizeof(buffer)), 0);
}

/**
 * @test Move-assigning into an already-open file closes the previous descriptor first.
 * @brief Proves the move-assign drop path: the destination's old fd is closed and only the
 *        incoming descriptor survives.
 */
TEST_F(FileSysTest, MoveAssignClosesPreviousDescriptor) {
    const std::filesystem::path second = test_dir / "second.txt";
    std::ofstream(second, std::ios::binary) << "second-file-content";

    qb::io::sys::file dst(test_file, O_RDONLY);
    ASSERT_TRUE(dst.is_open());

    qb::io::sys::file src(second, O_RDONLY);
    ASSERT_TRUE(src.is_open());
    const int src_handle = src.native_handle();

    dst = std::move(src); // dst's original fd is closed here; src is emptied
    EXPECT_FALSE(src.is_open());
    EXPECT_TRUE(dst.is_open());
    EXPECT_EQ(dst.native_handle(), src_handle);

    char      buffer[32] = {0};
    const int n          = dst.read(buffer, sizeof(buffer) - 1);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(n)), "second-file-content");
}

// =============================================================================
// ACCESS MODES
// =============================================================================

/**
 * @test O_RDONLY rejects writes, O_WRONLY rejects reads, O_APPEND appends to existing content.
 * @brief Salvaged from FileAccessModes; asserts the exact final content "test_append".
 */
TEST_F(FileSysTest, AccessModesEnforceDirectionAndAppend) {
    // Read-only: writing must fail.
    qb::io::sys::file ro;
    ASSERT_GE(ro.open(test_file, O_RDONLY), 0);
    EXPECT_LT(ro.write("test", 4), 0);
    ro.close();

    // Write-only: reading must fail, writing must succeed.
    const std::filesystem::path wo_path = test_dir / "write_only.txt";
    qb::io::sys::file           wo;
    ASSERT_GE(wo.open(wo_path, O_WRONLY | O_CREAT, 0644), 0);
    EXPECT_EQ(wo.write("test", 4), 4);
    char buffer[8] = {0};
    EXPECT_LT(wo.read(buffer, sizeof(buffer)), 0);
    wo.close();

    // Append mode: the new bytes go after the existing ones.
    qb::io::sys::file ap;
    ASSERT_GE(ap.open(wo_path, O_WRONLY | O_APPEND), 0);
    EXPECT_EQ(ap.write("_append", 7), 7);
    ap.close();

    EXPECT_EQ(slurp(wo_path), "test_append");
}

// =============================================================================
// EDGE CASES
// =============================================================================

/**
 * @test Empty file, 1-byte buffer, zero-size read/write, O_TRUNC.
 * @brief Salvaged from FileEdgeCases with exact-value assertions on every boundary.
 */
TEST_F(FileSysTest, EdgeCases) {
    // Empty file: a read returns 0 (EOF), not an error.
    const std::filesystem::path empty_file = test_dir / "empty.txt";
    std::ofstream(empty_file, std::ios::binary); // create, zero bytes

    qb::io::sys::file f;
    ASSERT_GE(f.open(empty_file, O_RDONLY), 0);
    char buffer[16] = {0};
    EXPECT_EQ(f.read(buffer, sizeof(buffer)), 0);
    f.close();

    // 1-byte buffer reads exactly the first byte of the content.
    ASSERT_GE(f.open(test_file, O_RDONLY), 0);
    char one = 0;
    EXPECT_EQ(f.read(&one, 1), 1);
    EXPECT_EQ(one, test_content[0]);
    f.close();

    // Zero-size read returns 0 with no side effects.
    ASSERT_GE(f.open(test_file, O_RDONLY), 0);
    EXPECT_EQ(f.read(buffer, 0), 0);
    f.close();

    // Zero-size write returns 0; the file remains empty under O_TRUNC.
    ASSERT_GE(f.open(empty_file, O_WRONLY | O_TRUNC), 0);
    EXPECT_EQ(f.write("", 0), 0);
    f.close();
    EXPECT_EQ(std::filesystem::file_size(empty_file), 0u);
}

/**
 * @test close() is idempotent.
 * @brief Closing an already-closed file is a safe no-op.
 */
TEST_F(FileSysTest, CloseIsIdempotent) {
    qb::io::sys::file f(test_file, O_RDONLY);
    ASSERT_TRUE(f.is_open());
    f.close();
    EXPECT_FALSE(f.is_open());
    f.close(); // must not crash or flip state
    EXPECT_FALSE(f.is_open());
}

// =============================================================================
// ERROR PATHS
// =============================================================================

/**
 * @test Opening a non-existent file fails; read/write on a closed fd return < 0.
 * @brief Salvaged from ErrorHandling.
 */
TEST_F(FileSysTest, ErrorPaths) {
    qb::io::sys::file f;
    f.open((test_dir / "does_not_exist.txt"), O_RDONLY);
    EXPECT_FALSE(f.is_open());

    char buffer[16] = {0};
    EXPECT_LT(f.read(buffer, sizeof(buffer)), 0);
    EXPECT_LT(f.write("test", 4), 0);
}

// =============================================================================
// DETERMINISTIC REPLACEMENT FOR ConcurrentOperations
// =============================================================================

/**
 * @test A reader sees an exact byte-prefix of what a writer has already committed.
 * @brief Replaces the flaky thread+sleep ConcurrentOperations test. Instead of racing a writer
 *        thread against a reader thread on wall-clock sleeps and asserting only `total_read > 0`,
 *        the writer fully flushes a known sequence of lines to disk (close() commits), then a
 *        fresh reader pulls a bounded chunk back and we assert the bytes read are an exact prefix
 *        of the bytes written — a real content check with zero timing dependence.
 */
TEST_F(FileSysTest, WriterThenReaderPrefixMatches) {
    const std::filesystem::path path = test_dir / "sequential.txt";

    std::string written;
    {
        qb::io::sys::file writer;
        ASSERT_GE(writer.open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), 0);
        for (int i = 0; i < 100; ++i) {
            const std::string line = "Line " + std::to_string(i) + "\n";
            ASSERT_EQ(writer.write(line.c_str(), line.size()), static_cast<int>(line.size()));
            written += line;
        }
        writer.close(); // commit before any read
    }

    ASSERT_FALSE(written.empty());
    EXPECT_EQ(std::filesystem::file_size(path), written.size());

    qb::io::sys::file reader;
    ASSERT_GE(reader.open(path, O_RDONLY), 0);

    char      chunk[256] = {0};
    const int n          = reader.read(chunk, sizeof(chunk));
    ASSERT_GT(n, 0);
    reader.close();

    // The bytes we read must be an exact prefix of the bytes we wrote — not merely non-empty.
    const std::string read_prefix(chunk, static_cast<std::size_t>(n));
    ASSERT_LE(read_prefix.size(), written.size());
    EXPECT_EQ(read_prefix, written.substr(0, read_prefix.size()));
}
