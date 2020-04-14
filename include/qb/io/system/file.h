/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include "../helper.h"
#include <fcntl.h>
#include <qb/system/allocator/pipe.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32

extern "C" {
int open(char const *pathname, int flags, ...);
}
#else
#    include <io.h>
#endif // !_WIN32

#ifndef QB_IO_SYS_FILE_H_
#    define QB_IO_SYS_FILE_H_

namespace qb::io::sys {

/*!
 * @class file sys/file.h qb/io/sys/file.h
 * @ingroup SYS
 */
class QB_API file {
    int _handle;

public:
    file() noexcept;
    file(file const &) = default;
    explicit file(int fd) noexcept;
    explicit file(std::string const &fname, int flags = O_RDWR) noexcept;

    [[nodiscard]] int ident() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void open(std::string const &fname, int flags = O_RDWR, int mode = 0644) noexcept;
    void open(int fd) noexcept;
    int write(const char *data, std::size_t size) const noexcept;
    int read(char *data, std::size_t size) const noexcept;
    void close() noexcept;

    // unused
    void
    setBlocking(bool) const noexcept {}
};

class QB_API file_to_pipe {
    qb::allocator::pipe<char> &_pipe;
    file _handle;
    std::size_t _expected_size = 0;
    std::size_t _read_bytes = 0;

public:
    file_to_pipe() = delete;
    ~file_to_pipe() noexcept;
    file_to_pipe(qb::allocator::pipe<char> &out) noexcept;

    bool open(std::string const &path) noexcept;
    [[nodiscard]] int read() noexcept;
    [[nodiscard]] int read_all() noexcept;
    [[nodiscard]] std::size_t read_bytes() const noexcept;
    [[nodiscard]] std::size_t expected_size() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool eof() const noexcept;
    void close() noexcept;
};

class QB_API pipe_to_file {
    const qb::allocator::pipe<char> &_pipe;
    file _handle;
    std::size_t _written_bytes = 0;

public:
    pipe_to_file() = delete;
    ~pipe_to_file() noexcept;
    pipe_to_file(qb::allocator::pipe<char> const &out) noexcept;

    bool open(std::string const &path, int mode = 0644) noexcept;
    [[nodiscard]] int write() noexcept;
    [[nodiscard]] int write_all() noexcept;
    [[nodiscard]] std::size_t written_bytes() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool eos() const noexcept;
    void close() noexcept;
};

} // namespace qb::io::sys

#endif // QB_IO_SYS_FILE_H_
