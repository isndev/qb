/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/file/file-pipe-transfer.cpp
 * @brief `qb::io::sys::file_to_pipe` / `pipe_to_file` — file ⇄ `qb::allocator::pipe<char>` bridges.
 *
 * `file_to_pipe` reads a whole file into a memory pipe (sizing itself with `fstat`); `pipe_to_file`
 * drains a memory pipe to a file. Both are synchronous local-filesystem helpers — no event loop,
 * no socket, no daemon — so this is a pure `unit` test. The historical suite proved the round-trip
 * across four near-duplicate size-escalating cases (FileToPipe, LargeFileOperations,
 * RoundTripOperations, VeryLargeFileTransfer); they are collapsed here into a single
 * size-parameterised `TEST_P` over {tiny, 1 MiB, 2 MiB} that drives the full
 * file → pipe → file → byte-identical round-trip at each size, plus the chunk-level
 * read()/write() progression and `eof()`/`eos()` contracts.
 *
 * The non-size-varying contracts (directory rejection, no-op reads/writes on a closed handle,
 * single vs. read_all, EOF idempotence, the gap-pipe and binary-with-NUL write, and `pipe_to_file`
 * partial-write resumption) are kept as their own focused cases.
 *
 * Restructured from the dissolved system/test-file-operations.cpp
 * (FileToPipe, FileToPipeRejectsDirectoriesAndClosedReadsAreNoops, PipeToFile,
 * PipeToFileClosedWriteAndEmptyPipeContracts, FileToPipeAdvanced, PipeToFileAdvanced,
 * RoundTripOperations, LargeFileOperations, VeryLargeFileTransfer). Per-file main() dropped for
 * the shared gtest main.
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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/system/file.h>
#include <qb/system/allocator/pipe.h>

namespace {

/// Read an entire file from disk as ground truth.
[[nodiscard]] std::string
slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Deterministic, diverse payload (printable cycle + embedded NULs) of an exact size.
[[nodiscard]] std::string
make_payload(std::size_t size) {
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        // Mix in NUL bytes so the binary path (not just text) is exercised.
        out.push_back((i % 64 == 0) ? '\0' : static_cast<char>('A' + (i % 26)));
    }
    return out;
}

} // namespace

// =============================================================================
// BASE FIXTURE — unique per-test scratch directory.
// =============================================================================

class FilePipeTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir;

    void
    SetUp() override {
        test_dir = std::filesystem::temp_directory_path() /
                   ("qb_file_pipe_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(test_dir);
        std::filesystem::create_directories(test_dir);
    }

    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }
};

// =============================================================================
// SIZE-PARAMETERISED ROUND-TRIP — collapses 4 size-escalating duplicates.
// =============================================================================

class FilePipeRoundTrip : public FilePipeTest, public ::testing::WithParamInterface<std::size_t> {};

/**
 * @test file → pipe → file is byte-identical at {tiny, 1 MiB, 2 MiB}.
 * @brief Single parameterised replacement for FileToPipe / LargeFileOperations /
 *        RoundTripOperations / VeryLargeFileTransfer. Proves: `expected_size()` equals the on-disk
 *        size; chunked `read()` makes monotonic progress and reaches `eof()` with exactly the file
 *        size buffered; the pipe content equals the source bytes exactly; `write_all()` drains the
 *        pipe to a file of identical size with `eos()` true; and the destination file is
 *        byte-for-byte equal to the source.
 */
TEST_P(FilePipeRoundTrip, FilePipeFileIsByteIdentical) {
    const std::size_t size = GetParam();
    const std::string payload = make_payload(size);

    const std::filesystem::path source = test_dir / "source.dat";
    std::ofstream(source, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_EQ(std::filesystem::file_size(source), size);

    // ---- file -> pipe (chunked read with progress assertions) ----------------
    qb::allocator::pipe<char> pipe;
    qb::io::sys::file_to_pipe f2p(pipe);
    ASSERT_TRUE(f2p.open(source));
    EXPECT_TRUE(f2p.is_open());
    EXPECT_EQ(f2p.expected_size(), size);

    std::size_t last_progress = 0;
    int         read_ops      = 0;
    while (!f2p.eof()) {
        const int n = f2p.read();
        ASSERT_GE(n, 0) << "read() must never return a hard error on local FS";
        if (n == 0)
            break;
        ++read_ops;
        EXPECT_GT(f2p.read_bytes(), last_progress) << "read() must make forward progress";
        last_progress = f2p.read_bytes();
    }
    EXPECT_GT(read_ops, 0);
    EXPECT_TRUE(f2p.eof());
    EXPECT_EQ(f2p.read_bytes(), size);
    ASSERT_EQ(pipe.size(), size);
    EXPECT_EQ(std::string(pipe.cbegin(), pipe.cbegin() + pipe.size()), payload);

    // ---- pipe -> file ---------------------------------------------------------
    const std::filesystem::path dest = test_dir / "dest.dat";
    qb::io::sys::pipe_to_file p2f(pipe);
    ASSERT_TRUE(p2f.open(dest));

    const int written = p2f.write_all();
    EXPECT_GE(written, 0);
    EXPECT_EQ(p2f.written_bytes(), size);
    EXPECT_TRUE(p2f.eos());
    p2f.close();

    EXPECT_EQ(std::filesystem::file_size(dest), size);
    EXPECT_EQ(slurp(dest), payload) << "round-tripped file must be byte-identical to the source";
}

INSTANTIATE_TEST_SUITE_P(Sizes,
                         FilePipeRoundTrip,
                         ::testing::Values(static_cast<std::size_t>(64),
                                           static_cast<std::size_t>(1024 * 1024),
                                           static_cast<std::size_t>(2 * 1024 * 1024)),
                         [](const ::testing::TestParamInfo<std::size_t> &info) {
                             switch (info.param) {
                                 case 64:
                                     return std::string("tiny");
                                 case 1024 * 1024:
                                     return std::string("one_mib");
                                 default:
                                     return std::string("two_mib");
                             }
                         });

// =============================================================================
// file_to_pipe — single-read vs read_all, EOF idempotence, error/no-op contracts.
// =============================================================================

/**
 * @test A single read() then read_all() leaves the pipe holding exactly the file, once.
 * @brief Salvaged from FileToPipeAdvanced. The first read() may take everything (platform
 *        dependent); read_all() must then be a 0-byte no-op, and a post-EOF read() returns 0.
 */
TEST_F(FilePipeTest, SingleReadThenReadAllThenEofIsIdempotent) {
    const std::filesystem::path file = test_dir / "content.txt";
    const std::string content = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-the-quick-brown-fox";
    std::ofstream(file, std::ios::binary) << content;

    qb::allocator::pipe<char> pipe;
    qb::io::sys::file_to_pipe f2p(pipe);
    ASSERT_TRUE(f2p.open(file));
    EXPECT_EQ(f2p.expected_size(), content.size());

    EXPECT_GT(f2p.read(), 0);
    // Whatever the first read consumed, read_all() finishes the job and the totals are exact.
    EXPECT_GE(f2p.read_all(), 0);
    EXPECT_EQ(f2p.read_bytes(), content.size());
    EXPECT_TRUE(f2p.eof());

    // Past EOF both read() and read_all() are 0-byte no-ops.
    EXPECT_EQ(f2p.read(), 0);
    EXPECT_EQ(f2p.read_all(), 0);

    EXPECT_EQ(std::string(pipe.cbegin(), pipe.cbegin() + pipe.size()), content);
}

/**
 * @test Opening a directory fails and reads on a never-opened handle are no-ops.
 * @brief Salvaged from FileToPipeRejectsDirectoriesAndClosedReadsAreNoops.
 */
TEST_F(FilePipeTest, RejectsDirectoryAndClosedReadsAreNoops) {
    qb::allocator::pipe<char> pipe;
    qb::io::sys::file_to_pipe f2p(pipe);

    EXPECT_FALSE(f2p.open(test_dir));
    EXPECT_FALSE(f2p.is_open());
    EXPECT_EQ(f2p.expected_size(), 0u);
    EXPECT_EQ(f2p.read(), 0);
    EXPECT_EQ(f2p.read_all(), 0);
    EXPECT_TRUE(f2p.eof());
    EXPECT_TRUE(pipe.empty());
}

/**
 * @test Opening a non-existent file fails.
 * @brief Salvaged from ErrorHandling (the file_to_pipe arm).
 */
TEST_F(FilePipeTest, OpenMissingFileFails) {
    qb::allocator::pipe<char> pipe;
    qb::io::sys::file_to_pipe f2p(pipe);
    EXPECT_FALSE(f2p.open(test_dir / "no_such_file.txt"));
    EXPECT_FALSE(f2p.is_open());
}

// =============================================================================
// pipe_to_file — closed/empty contracts, gap pipe, binary NULs, partial-write resume.
// =============================================================================

/**
 * @test Writes on a closed handle return -1; an opened-but-empty pipe yields a 0-byte file.
 * @brief Salvaged from PipeToFileClosedWriteAndEmptyPipeContracts.
 */
TEST_F(FilePipeTest, ClosedWriteAndEmptyPipeContracts) {
    qb::allocator::pipe<char> pipe;
    qb::io::sys::pipe_to_file p2f(pipe);

    EXPECT_FALSE(p2f.is_open());
    EXPECT_EQ(p2f.write(), -1);
    EXPECT_EQ(p2f.write_all(), -1);
    EXPECT_EQ(p2f.written_bytes(), 0u);
    EXPECT_TRUE(p2f.eos());

    const std::filesystem::path out = test_dir / "empty-pipe.txt";
    ASSERT_TRUE(p2f.open(out));
    EXPECT_TRUE(p2f.eos());
    EXPECT_EQ(p2f.write(), 0);
    EXPECT_EQ(p2f.write_all(), 0);
    EXPECT_EQ(std::filesystem::file_size(out), 0u);
}

/**
 * @test Opening an invalid path fails.
 * @brief Salvaged from ErrorHandling (the pipe_to_file arm).
 */
TEST_F(FilePipeTest, OpenInvalidPathFails) {
    qb::allocator::pipe<char> pipe;
    qb::io::sys::pipe_to_file p2f(pipe);
    EXPECT_FALSE(p2f.open("/invalid/path/that/does/not/exist/file.txt"));
}

/**
 * @test A pipe with a front-freed gap writes only the live bytes, contiguously.
 * @brief Salvaged from PipeToFileAdvanced (gap case). Proves the helper walks the pipe's logical
 *        view, not its raw allocation.
 */
TEST_F(FilePipeTest, WritesGappedPipeContiguously) {
    qb::allocator::pipe<char> pipe;

    const std::string seg1 = "First segment.";
    std::memcpy(pipe.allocate_back(seg1.size()), seg1.data(), seg1.size());
    const std::string seg2 = "Second segment.";
    std::memcpy(pipe.allocate_back(seg2.size()), seg2.data(), seg2.size());

    pipe.free_front(5); // drop "First" → live view begins at " segment.Second segment."

    const std::filesystem::path out = test_dir / "gap.txt";
    qb::io::sys::pipe_to_file p2f(pipe);
    ASSERT_TRUE(p2f.open(out));
    EXPECT_GT(p2f.write_all(), 0);
    EXPECT_TRUE(p2f.eos());
    p2f.close();

    EXPECT_EQ(slurp(out), " segment.Second segment.");
}

/**
 * @test Binary data with embedded NUL bytes survives the pipe → file write intact.
 * @brief Salvaged from PipeToFileAdvanced (binary case).
 */
TEST_F(FilePipeTest, WritesBinaryDataWithEmbeddedNuls) {
    const std::vector<char> data = {'B', 'I', 'N', 0, 'A', 'R', 'Y', 0, 'D', 'A', 'T', 'A'};

    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(data.size()), data.data(), data.size());

    const std::filesystem::path out = test_dir / "binary.bin";
    qb::io::sys::pipe_to_file p2f(pipe);
    ASSERT_TRUE(p2f.open(out));
    EXPECT_EQ(p2f.write_all(), static_cast<int>(data.size()));
    p2f.close();

    const std::string on_disk = slurp(out);
    ASSERT_EQ(on_disk.size(), data.size());
    EXPECT_TRUE(std::equal(data.begin(), data.end(), on_disk.begin()));
}

/**
 * @test write() resumes a partially-written pipe across repeated calls.
 * @brief New coverage requested by the dossier: the `while(!eos()) write()` loop is asserted at
 *        each step, proving write() advances `written_bytes()` monotonically and `eos()` flips
 *        exactly when every pipe byte has been committed.
 */
TEST_F(FilePipeTest, WriteResumesPartialPipeUntilEos) {
    const std::string payload = make_payload(256 * 1024);

    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(payload.size()), payload.data(), payload.size());

    const std::filesystem::path out = test_dir / "resumed.dat";
    qb::io::sys::pipe_to_file p2f(pipe);
    ASSERT_TRUE(p2f.open(out));

    std::size_t last = 0;
    int         write_ops = 0;
    while (!p2f.eos()) {
        const int n = p2f.write();
        ASSERT_GT(n, 0) << "write() must make progress on a non-empty pipe";
        ++write_ops;
        EXPECT_GT(p2f.written_bytes(), last);
        last = p2f.written_bytes();
    }
    EXPECT_GT(write_ops, 0);
    EXPECT_EQ(p2f.written_bytes(), payload.size());
    p2f.close();

    EXPECT_EQ(std::filesystem::file_size(out), payload.size());
    EXPECT_EQ(slurp(out), payload);
}
