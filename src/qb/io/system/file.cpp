/**
 * @file qb/io/system/file.cpp
 * @brief Implementation of the file I/O classes
 *
 * This file contains the implementation of classes for file I/O operations,
 * including direct file access, file-to-pipe and pipe-to-file transformations.
 * It provides cross-platform file operations for the QB framework.
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
 * @ingroup IO
 */

#include <algorithm>
#include <cerrno>
#include <limits>
#include <qb/io/system/file.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif

namespace qb::io::sys {

namespace {

// The errno of a file descriptor is the C runtime's, and its EBADF is 9 -- the value _read() / _write() set
// themselves. On Windows, qb/io/config.h re-maps EBADF to WSAEBADF (10009) for the socket layer, and that header is in
// scope here (qb-io builds as one translation unit), so the C runtime's value is spelled out rather than taken from
// the macro.
#if defined(_WIN32)
constexpr int file_errno_bad_descriptor = 9;
#else
constexpr int file_errno_bad_descriptor = EBADF;
#endif

} // namespace

#if defined(_WIN32)
namespace {

// The errno a failed CreateFileW() stands for (Huly QB-320): file::open() promises
// errno like POSIX open(), and the Win32 call reports through GetLastError() only.
// The table follows the CRT's own mapping for the codes an open can produce.
int
file_open_errno_from_win32(DWORD const err) noexcept {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return EACCES;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return EEXIST;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ENOMEM;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        default:
            return EIO;
    }
}

} // namespace
#endif

// file
file::file() noexcept
    : _handle(FD_INVALID) {}

file::file(int const fd) noexcept
    : _handle(fd) {}

file::file(std::filesystem::path const &fname, int const flags) noexcept
    : _handle(FD_INVALID) {
    open(fname, flags);
}

file::file(file &&other) noexcept
    : _handle(other._handle) {
    other._handle = FD_INVALID;
}

file &
file::operator=(file &&other) noexcept {
    if (this != &other) {
        close();
        _handle       = other._handle;
        other._handle = FD_INVALID;
    }
    return *this;
}

file::~file() noexcept {
    close();
}

int
file::native_handle() const noexcept {
    return _handle;
}

int
file::open(std::filesystem::path const &fname, int const flags, int const mode) noexcept {
    close();
#ifdef _WIN32
    // Translate the POSIX open() flags to CreateFile parameters so we can request
    // FILE_SHARE_DELETE. The CRT _open() opens without delete-sharing, which on
    // Windows blocks deleting or renaming the file while a descriptor is held —
    // POSIX allows unlink-while-open. Sharing delete restores that cross-platform
    // behaviour. The OS HANDLE is wrapped back into a CRT fd via _open_osfhandle()
    // so read()/write()/lseek()/close() below keep working unchanged.
    DWORD access;
    switch (flags & (_O_RDONLY | _O_WRONLY | _O_RDWR)) {
        case _O_WRONLY:
            access = GENERIC_WRITE;
            break;
        case _O_RDWR:
            access = GENERIC_READ | GENERIC_WRITE;
            break;
        default:
            access = GENERIC_READ;
            break; // _O_RDONLY == 0
    }
    if (flags & _O_APPEND)
        access |= FILE_APPEND_DATA;

    DWORD disposition;
    if (flags & _O_CREAT) {
        if (flags & _O_EXCL)
            disposition = CREATE_NEW;
        else if (flags & _O_TRUNC)
            disposition = CREATE_ALWAYS;
        else
            disposition = OPEN_ALWAYS;
    } else {
        disposition = (flags & _O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    }

    // fname.c_str() is a wide (wchar_t) native path on Windows — use CreateFileW for
    // proper Unicode path support (the previous CreateFileA truncated non-ANSI paths).
    const HANDLE h = ::CreateFileW(fname.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        _handle = FD_INVALID;
        errno   = file_open_errno_from_win32(::GetLastError());
    } else {
        // Preserve the descriptor's text/binary/append/read-only semantics; with
        // neither _O_TEXT nor _O_BINARY, _open_osfhandle() falls back to _fmode,
        // matching the previous _open() behaviour.
        int osf = 0;
        if ((flags & (_O_RDONLY | _O_WRONLY | _O_RDWR)) == _O_RDONLY)
            osf |= _O_RDONLY;
        if (flags & _O_APPEND)
            osf |= _O_APPEND;
        if (flags & _O_TEXT)
            osf |= _O_TEXT;
        if (flags & _O_BINARY)
            osf |= _O_BINARY;
        _handle = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), osf);
        if (_handle == FD_INVALID) {
            // _open_osfhandle() set errno (EMFILE); keep it across the cleanup.
            const int open_errno = errno;
            ::CloseHandle(h);
            errno = open_errno;
        }
    }
    (void) mode;
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
file::write(const char *data, std::size_t size) const noexcept {
    if (!is_open()) {
        errno = file_errno_bad_descriptor; // the errno the header promises, as write(-1, ...) would (Huly QB-320)
        return -1;
    }
    // Clamp to INT_MAX so the size never wraps when narrowed for the platform
    // syscall and the (int) return stays meaningful. Callers that need to move
    // more than 2 GiB loop on the return value (see pipe_to_file::write_all).
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        size = static_cast<std::size_t>(std::numeric_limits<int>::max());
#ifdef _WIN32
    return ::_write(_handle, data, static_cast<unsigned int>(size));
#else
    ssize_t ret;
    do {
        ret = ::write(_handle, data, size);
    } while (ret < 0 && errno == EINTR); // retry on signal interruption
    return static_cast<int>(ret);
#endif
}

int
file::read(char *data, std::size_t size) const noexcept {
    if (!is_open()) {
        errno = file_errno_bad_descriptor; // as read(-1, ...) would (Huly QB-320)
        return -1;
    }
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        size = static_cast<std::size_t>(std::numeric_limits<int>::max());
#ifdef _WIN32
    return ::_read(_handle, data, static_cast<unsigned int>(size));
#else
    ssize_t ret;
    do {
        ret = ::read(_handle, data, size);
    } while (ret < 0 && errno == EINTR); // retry on signal interruption
    return static_cast<int>(ret);
#endif
}

void
file::close() noexcept {
    if (is_open()) {
        const int fd = _handle;
        // Pre-invalidate the handle before calling the OS close.
        // This prevents re-entry and, critically on Windows, avoids a second
        // _close() call if two file objects share the same descriptor (copy
        // semantics): the second object will see _handle == FD_INVALID and
        // skip the call, preventing the CRT fast-fail (0xc0000409).
        _handle = FD_INVALID;
#ifdef _WIN32
        // _get_osfhandle() returns -1 without crashing for an already-closed
        // (or otherwise invalid) descriptor, letting us skip _close() safely.
        if (::_get_osfhandle(fd) != static_cast<intptr_t>(-1))
            ::_close(fd);
#else
        if (::close(fd))
            std::cerr << "Failed to close file" << std::endl;
#endif
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
file_to_pipe::open(std::filesystem::path const &path) noexcept {
    _handle.close();

    // Open the file first with O_NOFOLLOW to prevent TOCTOU race condition via symlinks
    // This ensures we operate on the actual file, not a swapped symlink
#ifdef O_NOFOLLOW
    _handle.open(path, O_RDONLY | O_NOFOLLOW);
#else
    // Fallback for platforms without O_NOFOLLOW
    _handle.open(path, O_RDONLY);
#endif

    if (!_handle.is_open()) {
        return false;
    }

    // Use fstat on the open file descriptor to get accurate file information
    // This eliminates the TOCTOU window between stat() and open()
    struct stat st;
    if (fstat(_handle.native_handle(), &st) != 0) {
        _handle.close();
        return false;
    }

    // Verify this is a regular file (not a directory, symlink, device, etc.)
    if (!S_ISREG(st.st_mode)) {
        _handle.close();
        return false;
    }

    _read_bytes    = 0;
    _expected_size = st.st_size;
    return true;
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
pipe_to_file::open(std::filesystem::path const &path, int const mode) noexcept {
    close();
    // O_TRUNC matches the documented contract and prevents stale trailing bytes
    // when overwriting an existing, larger file with a shorter payload.
    _handle.open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (is_open()) {
        _written_bytes = 0;
        return true;
    }
    return false;
}
int
pipe_to_file::write() noexcept {
    if (is_open()) {
        // Write only the not-yet-written remainder. Passing _pipe.size() here
        // (the previous code) read _written_bytes bytes past the end of the
        // pipe buffer after any partial write and duplicated/garbled output.
        const auto remaining = _pipe.size() - _written_bytes;
        auto       ret       = _handle.write(_pipe.cbegin() + _written_bytes, remaining);
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

// ---------------------------------------------------------------------------
// Executable location & resource resolution
// ---------------------------------------------------------------------------

std::filesystem::path
self_path() {
#if defined(_WIN32)
    // GetModuleFileNameW(nullptr) returns the path of the current process image.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return {}; // query failed
        if (len < buffer.size()) {
            buffer.resize(len);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2); // path longer than buffer: grow and retry
    }
#elif defined(__APPLE__)
    // Two-call idiom: first query the required size, then fill the buffer.
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer), ec);
    return ec ? std::filesystem::path(buffer) : canonical; // resolve the /./ and symlinks dyld may return
#else
    // Linux and other procfs systems expose the executable as a symlink.
    std::error_code       ec;
    std::filesystem::path link = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : link;
#endif
}

std::filesystem::path
self_dir() {
    const std::filesystem::path exe = self_path();
    return exe.empty() ? std::filesystem::path{} : exe.parent_path();
}

std::filesystem::path
resolve_resource(const std::filesystem::path &path) {
    if (path.is_absolute())
        return path;

    std::error_code ec;
    // 1. As given, relative to the current working directory (historical behaviour).
    if (std::filesystem::exists(path, ec))
        return path;

    // 2. Relative to the executable's own directory, so a binary shipped next to its
    //    assets resolves them from any working directory.
    if (const std::filesystem::path dir = self_dir(); !dir.empty()) {
        std::filesystem::path candidate = dir / path;
        if (std::filesystem::exists(candidate, ec))
            return candidate.lexically_normal(); // collapse the "/./" the join may introduce
    }

    // 3. Nothing matched: return the original so callers report what was requested.
    return path;
}

} // namespace qb::io::sys
