#ifndef QB_IO_TESTS_SHARED_FILE_DESCRIPTOR_PROBE_H
#define QB_IO_TESTS_SHARED_FILE_DESCRIPTOR_PROBE_H

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#endif

namespace qb::io::test {

// Snapshot while the fd is valid. Check before opening any unrelated resource.
// A replacement may reuse the closed descriptor/handle, so accept that identity
// only when it is the independently verified replacement owned by the file.
class file_descriptor_probe {
#ifdef _WIN32
    HANDLE _handle;

public:
    explicit file_descriptor_probe(int fd)
        : _handle(reinterpret_cast<HANDLE>(::_get_osfhandle(fd))) {}
    bool
    closed_or_reused_by(int replacement) const {
        if (_handle == reinterpret_cast<HANDLE>(::_get_osfhandle(replacement)))
            return true;
        DWORD flags = 0;
        return !::GetHandleInformation(_handle, &flags) && ::GetLastError() == ERROR_INVALID_HANDLE;
    }
#else
    int _fd;

public:
    explicit file_descriptor_probe(int fd)
        : _fd(fd) {}
    bool
    closed_or_reused_by(int replacement) const {
        return _fd == replacement || (::fcntl(_fd, F_GETFD) == -1 && errno == EBADF);
    }
#endif
};

} // namespace qb::io::test
#endif
