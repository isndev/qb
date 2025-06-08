/**
 * @file qb/io/src/system/file.cpp
 * @brief Implementation of the file I/O classes
 *
 * This file contains the implementation of classes for file I/O operations,
 * including direct file access, file-to-pipe and pipe-to-file transformations.
 * It provides cross-platform file operations for the QB framework.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * @ingroup IO
 */

#include <algorithm>
#include <qb/io/system/file.h>

namespace qb::io::sys {

// file
file::file() noexcept
    : _handle(FD_INVALID) {}

file::file(int const fd) noexcept
    : _handle(fd) {}

file::file(std::string const &fname, int const flags) noexcept
    : _handle(FD_INVALID) {
    open(fname, flags);
}

int
file::native_handle() const noexcept {
    return _handle;
}

int
file::open(std::string const &fname, int const flags, int const mode) noexcept {
    close();
#ifdef _WIN32
    _handle = ::_open(fname.c_str(), flags, mode);
#else
    _handle = ::open(fname.c_str(), flags, mode);
#endif
    return _handle;
}

int
file::open(int const fd) noexcept {
    close();
    _handle = fd;
    return _handle;
}

int
file::write(const char *data, std::size_t const size) const noexcept {
#ifdef _WIN32
    return ::_write(_handle, data, static_cast<unsigned int>(size));
#else
    return ::write(_handle, data, static_cast<unsigned int>(size));
#endif
}

int
file::read(char *data, std::size_t const size) const noexcept {
#ifdef _WIN32
    return ::_read(_handle, data, static_cast<unsigned int>(size));
#else
    return ::read(_handle, data, static_cast<unsigned int>(size));
#endif
}

void
file::close() noexcept {
    if (is_open()) {
#ifdef _WIN32
        auto ret = ::_close(_handle);
#else
        auto ret = ::close(_handle);
#endif

        if (!ret)
            _handle = FD_INVALID;
        else
            std::cerr << "Failed to close file" << std::endl;
    }
}

bool
file::is_open() const noexcept {
    return _handle != FD_INVALID;
}

// file_to_pipe
file_to_pipe::file_to_pipe(qb::allocator::pipe<char> &out) noexcept
    : _pipe(out) {}

file_to_pipe::~file_to_pipe() noexcept {
    _handle.close();
}

bool
file_to_pipe::open(std::string const &path) noexcept {
    struct stat st;

    _handle.close();
    if (!stat(path.c_str(), &st)) {
        _handle.open(path, O_RDONLY);
        if (_handle.is_open()) {
            _read_bytes    = 0;
            _expected_size = st.st_size;
            return true;
        }
    }
    return false;
}

int
file_to_pipe::read() noexcept {
    if (!eof() && is_open()) {
        const auto to_read = _expected_size - _read_bytes;
        const auto ret     = _handle.read(_pipe.allocate_back(to_read), to_read);
        if (ret < 0) {
            _pipe.free_back(to_read);
            close();
        } else {
            _pipe.free_back(to_read - ret);
            _read_bytes += ret;
        }

        return ret;
    }
    return 0;
}

int
file_to_pipe::read_all() noexcept {
    auto ret = 0;
    while ((ret = read()) > 0 && !eof())
        ;
    return ret;
}

std::size_t
file_to_pipe::read_bytes() const noexcept {
    return _read_bytes;
}

std::size_t
file_to_pipe::expected_size() const noexcept {
    return _expected_size;
}

bool
file_to_pipe::is_open() const noexcept {
    return _handle.is_open();
}

bool
file_to_pipe::eof() const noexcept {
    return _expected_size == _read_bytes;
}

void
file_to_pipe::close() noexcept {
    _handle.close();
}

// pipe_to_file

pipe_to_file::~pipe_to_file() noexcept {
    close();
}

pipe_to_file::pipe_to_file(qb::allocator::pipe<char> const &in) noexcept
    : _pipe(in) {}

bool
pipe_to_file::open(std::string const &path, int const mode) noexcept {
    close();
    _handle.open(path.c_str(), O_WRONLY | O_CREAT, mode);
    if (is_open()) {
        _written_bytes = 0;
        return true;
    }
    return false;
}
int
pipe_to_file::write() noexcept {
    if (is_open()) {
        auto ret = _handle.write(_pipe.cbegin() + _written_bytes, _pipe.size());
        if (ret < 0)
            close();
        else
            _written_bytes += ret;
        return ret;
    }
    return -1;
}

int
pipe_to_file::write_all() noexcept {
    auto ret = 0;
    while ((ret = write()) > 0 && !eos())
        ;
    return ret;
}

std::size_t
pipe_to_file::written_bytes() const noexcept {
    return _written_bytes;
}

bool
pipe_to_file::is_open() const noexcept {
    return _handle.is_open();
}

bool
pipe_to_file::eos() const noexcept {
    return written_bytes() == _pipe.size();
}

void
pipe_to_file::close() noexcept {
    _handle.close();
}

} // namespace qb::io::sys
