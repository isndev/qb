/**
 * @file qb/io/system/file.h
 * @brief File system operations and utilities for the QB IO library.
 *
 * This file provides classes for file system operations, including direct file access
 * (`sys::file`) and utilities for transferring data between files and memory pipes
 * (`sys::file_to_pipe`, `sys::pipe_to_file`). The implementation
 * supports both Windows and POSIX platforms with a consistent interface.
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
 * @ingroup FileSystem
 */

#include <fcntl.h>
#include <qb/system/allocator/pipe.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32

extern "C" {
int open(char const *pathname, int flags, ...);
}
#else
#include <io.h>
#endif // !_WIN32

#ifndef QB_IO_SYS_FILE_H_
#define QB_IO_SYS_FILE_H_

namespace qb::io::sys {

/**
 * @class file
 * @ingroup FileSystem
 * @brief Wrapper class for platform-independent file system operations using native file descriptors.
 *
 * This class provides a platform-independent interface for basic file operations
 * such as opening, reading, writing, and closing files. It uses native file
 * descriptors internally and works on both Windows and POSIX platforms.
 * It is primarily for synchronous file I/O.
 */
class QB_API file {
    int _handle; /**< Native file descriptor handle. `-1` if not open. */

public:
    /**
     * @brief Default constructor.
     * Creates a file object with an invalid file descriptor (`-1`).
     * Call `open()` to associate it with a file.
     */
    file() noexcept;

    /**
     * @brief Copy constructor (defaulted, copies the file descriptor).
     * @note Copying a file object results in shared ownership of the underlying file descriptor.
     *       Closing one copied object will affect others if they share the same handle.
     *       Consider using unique ownership or move semantics for safer resource management.
     */
    file(file const &) = default;

    /**
     * @brief Constructor from an existing native file descriptor.
     * @param fd Existing native file descriptor to wrap. The `file` object takes conceptual ownership
     *           but does not validate if `fd` is a valid open descriptor.
     */
    explicit file(int fd) noexcept;

    /**
     * @brief Constructor that opens a file by name.
     * @param fname Path to the file to open.
     * @param flags File open flags (e.g., `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`). Defaults to `O_RDWR`.
     *              These are standard POSIX flags; Windows equivalents are used internally.
     * @details Calls `open(fname, flags)` internally. If opening fails, `_handle` will be `-1`.
     */
    explicit file(std::string const &fname, int flags = O_RDWR) noexcept;

    /**
     * @brief Returns the native file descriptor handle.
     * @return The internal file descriptor, or `-1` if the file is not open.
     */
    [[nodiscard]] int native_handle() const noexcept;

    /**
     * @brief Checks if the file is currently open and associated with a valid descriptor.
     * @return `true` if the file is open (`_handle != -1`), `false` otherwise.
     */
    [[nodiscard]] bool is_open() const noexcept;

    /**
     * @brief Opens a file with the specified path, flags, and mode.
     * @param fname Path to the file to open.
     * @param flags File open flags (e.g., `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_APPEND`, `O_TRUNC`).
     *              Defaults to `O_RDWR`. These are standard POSIX flags.
     * @param mode File creation mode (permissions) if `O_CREAT` is used (e.g., `0644`).
     *             Defaults to `0644` (rw-r--r--). Ignored if `O_CREAT` is not in flags.
     * @return The native file descriptor if successful, or `-1` on error (errno is set).
     * @details If the file object already holds an open descriptor, it is closed first.
     */
    int open(std::string const &fname, int flags = O_RDWR, int mode = 0644) noexcept;

    /**
     * @brief Associates this file object with an existing native file descriptor.
     * @param fd The native file descriptor to use.
     * @return The provided file descriptor `fd`.
     * @details If this `file` object already holds an open descriptor, it is closed first.
     *          The object then wraps the provided `fd`.
     */
    int open(int fd) noexcept;

    /**
     * @brief Writes data to the file from the current file position.
     * @param data Pointer to the buffer containing data to write.
     * @param size Number of bytes to write from the buffer.
     * @return Number of bytes actually written. This can be less than `size` if an error
     *         occurs or if the disk is full. Returns a negative value on error (errno is set).
     */
    int write(const char *data, std::size_t size) const noexcept;

    /**
     * @brief Reads data from the file from the current file position.
     * @param data Buffer to store the read data.
     * @param size Maximum number of bytes to read (size of the `data` buffer).
     * @return Number of bytes actually read. `0` indicates end-of-file.
     *         A negative value indicates an error (errno is set).
     */
    int read(char *data, std::size_t size) const noexcept;

    /**
     * @brief Closes the file.
     * @details This method closes the underlying native file descriptor and resets the internal `_handle` to `-1`.
     *          If the file is already closed, this method does nothing. It is safe to call multiple times.
     */
    void close() noexcept;

    /**
     * @brief Placeholder for setting non-blocking mode (currently a no-op for `sys::file`).
     * @param nonblocking Unused boolean parameter.
     * @details Standard file descriptors are typically blocking. Asynchronous file operations
     *          in qb-io are usually handled by `file_watcher` or by offloading blocking calls
     *          to separate threads/callbacks rather than setting file descriptors to non-blocking.
     */
    void
    set_non_blocking(bool /*nonblocking*/) const noexcept {} // Parameter marked as unused
};

/**
 * @class file_to_pipe
 * @ingroup FileSystem
 * @brief Utility for efficiently reading the entire contents of a file into a `qb::allocator::pipe<char>`.
 *
 * This class provides functionality for transferring data from a file specified by path
 * into a memory pipe, typically for further processing or network transfer.
 * It tracks the read progress and the total expected size of the file.
 */
class QB_API file_to_pipe {
    qb::allocator::pipe<char> &_pipe;   /**< Reference to the destination memory pipe. */
    file                       _handle; /**< Internal `sys::file` handle for reading. */
    std::size_t _expected_size = 0;     /**< Total size of the file in bytes, determined on open. */
    std::size_t _read_bytes    = 0;     /**< Number of bytes already read from the file into the pipe. */

public:
    /**
     * @brief Default constructor is deleted. A destination pipe must be provided.
     */
    file_to_pipe() = delete;

    /**
     * @brief Destructor.
     * Automatically closes the file via `_handle`'s destructor if it's still open.
     */
    ~file_to_pipe() noexcept;

    /**
     * @brief Constructor.
     * @param out Reference to the `qb::allocator::pipe<char>` that will receive the file data.
     */
    file_to_pipe(qb::allocator::pipe<char> &out) noexcept;

    /**
     * @brief Opens a file for reading its content into the pipe.
     * @param path Path to the file to open. The file must exist and be readable.
     * @return `true` if the file was successfully opened and its size determined, `false` otherwise (e.g., file not found, no permissions).
     * @details This method also retrieves the file size using `fstat` (or equivalent) to set `_expected_size`
     *          and resets internal read progress counters.
     */
    bool open(std::string const &path) noexcept;

    /**
     * @brief Reads a chunk of data from the file into the associated pipe.
     * @return Number of bytes read in this operation. `0` if EOF was already reached or if `_read_bytes == _expected_size`.
     *         A negative value indicates a read error from the underlying `sys::file::read`.
     * @details This method reads a chunk of data (typically up to the pipe's internal buffer capacity or remaining file size)
     *          from the file to the pipe and updates `_read_bytes`.
     */
    [[nodiscard]] int read() noexcept;

    /**
     * @brief Reads all remaining data from the file into the pipe.
     * @return The result of the last read operation (bytes read, or 0 if fully read, or negative on error).
     * @details This method repeatedly calls `read()` until `eof()` is true or an error occurs.
     */
    [[nodiscard]] int read_all() noexcept;

    /**
     * @brief Returns the number of bytes read from the file so far.
     * @return Count of bytes successfully read and enqueued into the pipe.
     */
    [[nodiscard]] std::size_t read_bytes() const noexcept;

    /**
     * @brief Returns the total expected size of the file (determined when opened).
     * @return Total size of the file in bytes.
     */
    [[nodiscard]] std::size_t expected_size() const noexcept;

    /**
     * @brief Checks if the underlying file handle is open.
     * @return `true` if the file is open, `false` otherwise.
     */
    [[nodiscard]] bool is_open() const noexcept;

    /**
     * @brief Checks if all expected file data has been read into the pipe.
     * @return `true` if `_read_bytes == _expected_size` and the file is open, `false` otherwise.
     */
    [[nodiscard]] bool eof() const noexcept;

    /**
     * @brief Closes the underlying file handle.
     * @details Calls `_handle.close()`.
     */
    void close() noexcept;
};

/**
 * @class pipe_to_file
 * @ingroup FileSystem
 * @brief Utility for efficiently writing the contents of a `qb::allocator::pipe<char>` to a file.
 *
 * This class provides functionality for transferring data from a memory pipe
 * to a file specified by path. It tracks the write progress.
 */
class QB_API pipe_to_file {
    const qb::allocator::pipe<char> &_pipe;   /**< Reference to the source memory pipe. */
    file                             _handle; /**< Internal `sys::file` handle for writing. */
    std::size_t _written_bytes = 0;           /**< Number of bytes already written from the pipe to the file. */

public:
    /**
     * @brief Default constructor is deleted. A source pipe must be provided.
     */
    pipe_to_file() = delete;

    /**
     * @brief Destructor.
     * Automatically closes the file via `_handle`'s destructor if it's still open.
     */
    ~pipe_to_file() noexcept;

    /**
     * @brief Constructor.
     * @param in Reference to the constant `qb::allocator::pipe<char>` containing the data to write.
     */
    pipe_to_file(qb::allocator::pipe<char> const &in) noexcept; // Corrected to 'in'

    /**
     * @brief Opens a file for writing the pipe contents.
     * @param path Path to the file to open/create. If the file exists, it is typically truncated unless specific flags are used.
     * @param mode File creation mode (permissions, e.g., `0644`) if the file is created.
     *             Defaults to `0644` (rw-r--r--).
     * @return `true` if the file was successfully opened for writing, `false` otherwise.
     * @details Opens the file with `O_WRONLY | O_CREAT | O_TRUNC` by default (platform equivalents).
     *          Resets internal write progress counters.
     */
    bool open(std::string const &path, int mode = 0644) noexcept;

    /**
     * @brief Writes a chunk of data from the associated pipe to the file.
     * @return Number of bytes written in this operation from the pipe's current read position.
     *         A negative value indicates a write error from the underlying `sys::file::write`.
     * @details This method attempts to write a chunk of data from the pipe (starting from its current effective beginning)
     *          to the file and updates `_written_bytes`.
     */
    [[nodiscard]] int write() noexcept;

    /**
     * @brief Writes all remaining data from the pipe to the file.
     * @return The result of the last write operation (bytes written, or negative on error).
     * @details This method repeatedly calls `write()` until all data in the pipe has been written
     *          (i.e., `eos()` is true) or an error occurs.
     */
    [[nodiscard]] int write_all() noexcept;

    /**
     * @brief Returns the number of bytes written to the file so far.
     * @return Count of bytes successfully written from the pipe.
     */
    [[nodiscard]] std::size_t written_bytes() const noexcept;

    /**
     * @brief Checks if the underlying file handle is open.
     * @return `true` if the file is open, `false` otherwise.
     */
    [[nodiscard]] bool is_open() const noexcept;

    /**
     * @brief Checks if all data from the pipe has been written to the file.
     * @return `true` if `_written_bytes == _pipe.size()` and the file is open, `false` otherwise.
     */
    [[nodiscard]] bool eos() const noexcept;

    /**
     * @brief Closes the underlying file handle.
     * @details Calls `_handle.close()`.
     */
    void close() noexcept;
};

} // namespace qb::io::sys

#endif // QB_IO_SYS_FILE_H_
