/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/system/sys__socket.h>
#include <qb/system/allocator/pipe.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

int qb_test_write(int fd, const void *data, std::size_t size);

// Compile the real implementation in this executable only. Preloading its system
// headers above confines replacement to file.h/file.cpp: native write/_write and
// the two write method names become qb_test_write. No symbols from this private
// copy are linked with qb-io, and every method body is the production source.
#define write qb_test_write
#define _write qb_test_write
#include <qb/io/system/file.cpp>
#undef _write
#undef write

namespace file_short_writes_test {

struct WriteScript {
    const char              *begin;
    std::size_t              total;
    std::size_t              max_write;
    std::size_t              committed = 0;
    std::vector<std::size_t> requests;
};

thread_local WriteScript *active_script = nullptr;

class ScopedScript {
    WriteScript *_previous;

public:
    explicit ScopedScript(WriteScript &script)
        : _previous(active_script) {
        active_script = &script;
    }
    ~ScopedScript() {
        active_script = _previous;
    }
    ScopedScript(const ScopedScript &)            = delete;
    ScopedScript &operator=(const ScopedScript &) = delete;
};

} // namespace file_short_writes_test

int
qb_test_write(int fd, const void *data, std::size_t size) {
    auto *script = file_short_writes_test::active_script;
    if (!script) {
        ADD_FAILURE() << "write called without its owned script";
        errno = EIO;
        return -1;
    }
    script->requests.push_back(size);
    const auto remaining = script->total - script->committed;
    EXPECT_EQ(data, script->begin + script->committed);
    EXPECT_EQ(size, remaining) << "each resumed write must request only the unwritten suffix";
    if (data != script->begin + script->committed || size != remaining) {
        // Reject before reading any bytes: the historical length bug must fail
        // as an oracle violation, without relying on an out-of-bounds native read.
        errno = EIO;
        return -1;
    }
    const auto accepted = std::min(size, script->max_write);
#ifdef _WIN32
    const int result = ::_write(fd, data, static_cast<unsigned int>(accepted));
#else
    const int result = static_cast<int>(::write(fd, data, accepted));
#endif
    if (result > 0)
        script->committed += static_cast<std::size_t>(result);
    return result;
}

namespace file_short_writes_test {

class FileShortWrites : public ::testing::Test {
protected:
    std::filesystem::path directory;

    void
    SetUp() override {
        directory = std::filesystem::temp_directory_path()
                    / ("qb_file_short_write_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_"
                       + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
    }
    void
    TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
};

TEST_F(FileShortWrites, ResumeRequestsRemainingSuffixAndWritesExactBytes) {
    const std::string         payload{"abcdefghij\0klmnopqrstuv", 23};
    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(payload.size()), payload.data(), payload.size());
    qb::io::sys::pipe_to_file writer{pipe};
    const auto                destination = directory / "short.dat";
    ASSERT_TRUE(writer.open(destination));
    WriteScript        script{pipe.cbegin(), pipe.size(), 7, 0, {}};
    const ScopedScript scope{script};

    ASSERT_EQ(writer.qb_test_write(), 7);
    EXPECT_EQ(writer.written_bytes(), 7u);
    EXPECT_FALSE(writer.eos());
    ASSERT_EQ(writer.qb_test_write(), 7);
    EXPECT_EQ(writer.written_bytes(), 14u);
    EXPECT_FALSE(writer.eos());
    EXPECT_EQ(writer.write_all(), 2);
    EXPECT_TRUE(writer.eos());
    EXPECT_EQ(writer.written_bytes(), payload.size());
    EXPECT_EQ(script.requests, (std::vector<std::size_t>{23, 16, 9, 2}));
    writer.close();

    std::ifstream in(destination, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>{in}, {}), payload);
}

TEST_F(FileShortWrites, FullWriteNeedsOneOperation) {
    const std::string         payload = "one complete write";
    qb::allocator::pipe<char> pipe;
    std::memcpy(pipe.allocate_back(payload.size()), payload.data(), payload.size());
    qb::io::sys::pipe_to_file writer{pipe};
    ASSERT_TRUE(writer.open(directory / "full.dat"));
    WriteScript        script{pipe.cbegin(), pipe.size(), payload.size(), 0, {}};
    const ScopedScript scope{script};

    EXPECT_EQ(writer.write_all(), static_cast<int>(payload.size()));
    EXPECT_TRUE(writer.eos());
    EXPECT_EQ(script.requests, (std::vector<std::size_t>{payload.size()}));
}

} // namespace file_short_writes_test
