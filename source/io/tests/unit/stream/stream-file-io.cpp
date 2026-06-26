/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/stream/stream-file-io.cpp
 * @brief `qb::io::{istream,ostream,stream}<sys::file>` and `transport::file` over the local filesystem.
 *
 * Where stream-templates.cpp drives the buffering templates against a scripted in-memory transport,
 * this file binds them to the real `qb::io::sys::file` transport (qb/io/system/file.h) and the
 * `qb::io::transport::file` adapter (qb/io/transport/file.h). Every case is local-filesystem only —
 * no network, no event loop, no daemon — so it is `unit`. `sys::file` is move-only with a closing
 * destructor, so `stream.transport() = std::move(file)` transfers the descriptor and leaves the
 * local handle closed; the streams then own the fd.
 *
 * Contracts proven:
 *   - input  stream: read a file into the buffer and compare to ground truth;
 *   - output stream: publish + write commits to disk (verified via std::ifstream), for both a
 *     string and a std::vector source;
 *   - bidirectional stream: write then re-open + read the same bytes back;
 *   - transport::file: its overridden write() is a deliberate no-op returning 0 (writing is done via
 *     the underlying sys::file), while read() pulls bytes into the inherited input buffer;
 *   - error path: read/write on a stream wrapping a closed (default-constructed) sys::file return < 0;
 *   - stream chaining: file → istream buffer → ostream → file, byte-for-byte;
 *   - stale-handle resilience: deleting the input file mid-stream does not break a subsequent
 *     independent output stream (and the already-buffered read result is asserted, not masked);
 *   - stream composition: a TransformStream adapter applies a byte transform on read and on publish.
 *
 * Restructured from the dissolved system/test-stream-operations.cpp StreamTest cases
 * (FileInputStream, FileOutputStream, FileBidirectionalStream, FileTransport, StreamErrors,
 * StreamChaining, AdvancedErrorHandling, StreamComposition). Every `if(result<=0){cout<<"Warning"}`
 * mask and the StreamChaining early-`return` are replaced with hard `ASSERT_GT` — local file I/O does
 * not legitimately fail on supported platforms, so a failure is a regression that must be loud.
 * AdvancedErrorHandling is rescoped to actually ASSERT the buffered read result (it previously only
 * smoke-checked recovery). TransformStream now comes from the shared header. Per-file main() dropped.
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

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/stream.h>
#include <qb/io/system/file.h>
#include <qb/io/transport/file.h>

#include "../../shared/scripted_stream_transport.h"

using qb::io::test::TransformStream;

namespace {

/// Read an entire file from disk as ground truth.
[[nodiscard]] std::string
slurp(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

// =============================================================================
// TEST FIXTURE — unique per-test scratch directory with a seeded input file.
// =============================================================================

class StreamFileIoTest : public ::testing::Test {
protected:
    std::filesystem::path test_dir;
    std::filesystem::path test_file;
    const std::string     test_content = "Hello, QB Stream Test!";

    void
    SetUp() override {
        test_dir = std::filesystem::temp_directory_path() /
                   ("qb_stream_file_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(test_dir);
        std::filesystem::create_directories(test_dir);

        test_file = test_dir / "stream_test.txt";
        std::ofstream(test_file, std::ios::binary) << test_content;
    }

    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }
};

// =============================================================================
// INPUT STREAM
// =============================================================================

/**
 * @test istream<sys::file> reads a file into its buffer.
 * @brief Salvaged from FileInputStream; the masked `if(read<=0) cout<<Warning` is now a hard ASSERT.
 */
TEST_F(StreamFileIoTest, FileInputStream) {
    qb::io::sys::file file;
    ASSERT_GE(file.open(test_file, O_RDONLY), 0);
    ASSERT_TRUE(file.is_open());

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(file);

    const int read_result = input_stream.read();
    ASSERT_GT(read_result, 0) << "reading a local file must not fail";

    ASSERT_GE(input_stream.in().size(), test_content.size());
    const std::string read_content(input_stream.in().data(), test_content.size());
    EXPECT_EQ(read_content, test_content);

    input_stream.close();
}

// =============================================================================
// OUTPUT STREAM
// =============================================================================

/**
 * @test ostream<sys::file> writes a string then a vector source, verified against disk.
 * @brief Salvaged from FileOutputStream.
 */
TEST_F(StreamFileIoTest, FileOutputStream) {
    const std::filesystem::path out = test_dir / "output.txt";

    qb::io::sys::file file;
    ASSERT_GE(file.open(out, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(file);

    const std::string write_content = "Testing output stream";
    ASSERT_NE(output_stream.publish(write_content.c_str(), write_content.size()), nullptr);
    ASSERT_GT(output_stream.write(), 0);
    output_stream.close();

    EXPECT_EQ(slurp(out), write_content);

    // Second pass: a std::vector source over a truncated file.
    qb::io::sys::file file2;
    ASSERT_GE(file2.open(out, O_WRONLY | O_TRUNC, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream2;
    output_stream2.transport() = std::move(file2);

    const std::string vec_content = "Vector content test";
    const std::vector<char> vec_buffer(vec_content.begin(), vec_content.end());
    ASSERT_NE(output_stream2.publish(vec_buffer.data(), vec_buffer.size()), nullptr);
    ASSERT_GT(output_stream2.write(), 0);
    output_stream2.close();

    EXPECT_EQ(slurp(out), vec_content);
}

// =============================================================================
// BIDIRECTIONAL STREAM
// =============================================================================

/**
 * @test stream<sys::file> writes then reads the same bytes back from disk.
 * @brief Salvaged from FileBidirectionalStream; both masked warning branches are now hard ASSERTs.
 */
TEST_F(StreamFileIoTest, FileBidirectionalStream) {
    const std::filesystem::path bidir = test_dir / "bidir.txt";

    qb::io::sys::file file;
    ASSERT_GE(file.open(bidir, O_RDWR | O_CREAT, 0644), 0);

    qb::io::stream<qb::io::sys::file> bidir_stream;
    bidir_stream.transport() = std::move(file);

    const std::string write_content = "Bidirectional stream test";
    ASSERT_NE(bidir_stream.publish(write_content.c_str(), write_content.size()), nullptr);
    ASSERT_GT(bidir_stream.write(), 0) << "writing a local file must not fail";
    bidir_stream.close();

    // Re-open to reset the file position, then read it back.
    qb::io::sys::file reopened;
    ASSERT_GE(reopened.open(bidir, O_RDWR), 0);
    bidir_stream.transport() = std::move(reopened);

    const int read_result = bidir_stream.read();
    ASSERT_GT(read_result, 0) << "reading a local file must not fail";

    ASSERT_GE(bidir_stream.in().size(), write_content.size());
    const std::string read_content(bidir_stream.in().data(), write_content.size());
    EXPECT_EQ(read_content, write_content);

    bidir_stream.close();
}

// =============================================================================
// transport::file ADAPTER
// =============================================================================

/**
 * @test transport::file::write() is a deliberate no-op (returns 0); read() fills the input buffer.
 * @brief Salvaged from FileTransport, de-masked and corrected. The original test masked the fact
 *        that `transport::file` overrides `write()` to ALWAYS return 0 (writing is handled via the
 *        underlying sys::file, not the stream's write()): its `if(write_result<=0) cout<<Warning`
 *        branch silently skipped the verification, so the no-op behaviour was never asserted. Here we
 *        assert the contract directly — publish() buffers the bytes but write() commits nothing and
 *        returns 0 — and separately exercise the working read() path the adapter inherits.
 */
TEST_F(StreamFileIoTest, FileTransportWriteIsNoopAndReadFillsBuffer) {
    const std::filesystem::path path = test_dir / "transport.txt";

    // ---- write() is a no-op returning 0 -------------------------------------
    {
        qb::io::sys::file file;
        ASSERT_GE(file.open(path, O_WRONLY | O_CREAT, 0644), 0);

        qb::io::transport::file transport;
        transport.transport() = std::move(file);

        const std::string write_content = "Transport file test";
        ASSERT_NE(transport.publish(write_content.c_str(), write_content.size()), nullptr);
        EXPECT_EQ(transport.pendingWrite(), write_content.size());

        // The overridden write() commits nothing and returns 0; pending data is unchanged.
        EXPECT_EQ(transport.write(), 0) << "transport::file::write() is a documented no-op";
        EXPECT_EQ(transport.pendingWrite(), write_content.size());
        transport.close();
    }
    // Nothing was written through the adapter's write().
    EXPECT_EQ(std::filesystem::file_size(path), 0u);

    // ---- read() fills the inherited input buffer ----------------------------
    // Seed the file directly, then read it back through the adapter.
    const std::string seeded = "Transport read path";
    std::ofstream(path, std::ios::binary) << seeded;

    qb::io::sys::file rfile;
    ASSERT_GE(rfile.open(path, O_RDONLY), 0);

    qb::io::transport::file rtransport;
    rtransport.transport() = std::move(rfile);

    const int read_result = rtransport.read();
    ASSERT_GT(read_result, 0) << "reading a local file must not fail";
    ASSERT_GE(rtransport.in().size(), seeded.size());
    EXPECT_EQ(std::string(rtransport.in().data(), seeded.size()), seeded);
    rtransport.close();
}

// =============================================================================
// ERROR PATH
// =============================================================================

/**
 * @test Read/write on a stream wrapping a closed sys::file return < 0.
 * @brief Salvaged from StreamErrors — a SOLID negative test of the move-only closed-fd behaviour.
 */
TEST_F(StreamFileIoTest, ClosedTransportReadWriteFail) {
    qb::io::istream<qb::io::sys::file> input_stream;
    {
        qb::io::sys::file closed_file; // default-constructed → fd == -1
        input_stream.transport() = std::move(closed_file);
    }
    EXPECT_LT(input_stream.read(), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    {
        qb::io::sys::file closed_file;
        output_stream.transport() = std::move(closed_file);
    }
    const std::string data = "Test data";
    ASSERT_NE(output_stream.publish(data.c_str(), data.size()), nullptr);
    EXPECT_LT(output_stream.write(), 0);
}

// =============================================================================
// STREAM CHAINING
// =============================================================================

/**
 * @test file → istream buffer → ostream → file is byte-identical.
 * @brief Salvaged from StreamChaining; the early `return` on a masked read failure is now a hard
 *        ASSERT so a regression fails loudly instead of silently skipping the verification.
 */
TEST_F(StreamFileIoTest, StreamChaining) {
    const std::filesystem::path source_file = test_dir / "source_chain.txt";
    const std::filesystem::path dest_file   = test_dir / "dest_chain.txt";
    const std::string content = "Testing stream chaining with non-trivial content 12345!@#$%";
    std::ofstream(source_file, std::ios::binary) << content;

    qb::io::sys::file file_source;
    ASSERT_GE(file_source.open(source_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(file_source);

    const int read_result = input_stream.read();
    ASSERT_GT(read_result, 0) << "reading the source file must not fail";

    qb::io::sys::file file_dest;
    ASSERT_GE(file_dest.open(dest_file, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(file_dest);

    ASSERT_NE(output_stream.publish(input_stream.in().data(), input_stream.in().size()), nullptr);
    ASSERT_GT(output_stream.write(), 0) << "writing the destination file must not fail";

    input_stream.close();
    output_stream.close();

    EXPECT_EQ(slurp(dest_file), content);
}

// =============================================================================
// STALE-HANDLE RESILIENCE
// =============================================================================

/**
 * @test A stream survives its input file being deleted mid-flight; a fresh output stream still works.
 * @brief Rescoped from AdvancedErrorHandling. The original only smoke-checked recovery; here we
 *        ASSERT the buffered read result (the bytes captured before deletion) AND prove a subsequent
 *        independent output stream commits correctly even though the input file was unlinked.
 */
TEST_F(StreamFileIoTest, DeletedInputFileDoesNotBreakSubsequentOutput) {
    const std::filesystem::path temp_file = test_dir / "temp_delete.txt";
    const std::string seeded = "This file will be deleted during read/write operations";
    std::ofstream(temp_file, std::ios::binary) << seeded;

    qb::io::sys::file read_file;
    ASSERT_GE(read_file.open(temp_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(read_file);

    const int read_result = input_stream.read();
    ASSERT_GT(read_result, 0) << "the pre-delete read must succeed";
    // The bytes captured before deletion are exactly the seeded content.
    ASSERT_GE(input_stream.in().size(), seeded.size());
    EXPECT_EQ(std::string(input_stream.in().data(), seeded.size()), seeded);

    // Unlink the input file while the stream still holds the (now-stale) descriptor.
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::remove(temp_file, ec));

    // A fresh, independent output stream must still work.
    const std::filesystem::path out = test_dir / "after_delete.txt";
    qb::io::sys::file write_file;
    ASSERT_GE(write_file.open(out, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(write_file);

    const std::string recovery = "Data written after error";
    ASSERT_NE(output_stream.publish(recovery.c_str(), recovery.size()), nullptr);
    ASSERT_GT(output_stream.write(), 0);
    output_stream.close();
    input_stream.close();

    EXPECT_EQ(slurp(out), recovery);
}

// =============================================================================
// STREAM COMPOSITION
// =============================================================================

/**
 * @test A TransformStream adapter uppercases bytes on read and on publish.
 * @brief Salvaged from StreamComposition; TransformStream now comes from the shared header.
 */
TEST_F(StreamFileIoTest, StreamComposition) {
    const std::filesystem::path source_file = test_dir / "transform_source.txt";
    const std::filesystem::path dest_file   = test_dir / "transform_dest.txt";
    const std::string content = "abcdefghijklmnopqrstuvwxyz";
    std::ofstream(source_file, std::ios::binary) << content;

    const auto uppercase = [](char *data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            if (data[i] >= 'a' && data[i] <= 'z')
                data[i] = static_cast<char>(data[i] - 'a' + 'A');
    };

    // ---- read + transform ----------------------------------------------------
    qb::io::sys::file source;
    ASSERT_GE(source.open(source_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(source);

    TransformStream<qb::io::istream<qb::io::sys::file>> transform_in(input_stream, uppercase);
    ASSERT_GT(transform_in.read(), 0);

    const std::string transformed(transform_in.in().data(), transform_in.in().size());
    EXPECT_EQ(transformed, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    // ---- transform + write ---------------------------------------------------
    qb::io::sys::file dest;
    ASSERT_GE(dest.open(dest_file, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(dest);

    TransformStream<qb::io::ostream<qb::io::sys::file>> transform_out(output_stream, uppercase);
    transform_out.publish("testing transformation", 22);
    ASSERT_GT(transform_out.write(), 0);

    input_stream.close();
    output_stream.close();

    EXPECT_EQ(slurp(dest_file), "TESTING TRANSFORMATION");
}
