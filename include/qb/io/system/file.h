/**
 * @file qb/io/system/file.h
 * @brief File system operations and utilities
 * 
 * This file provides classes for file system operations, including direct file access
 * and utilities for transferring data between files and memory pipes. The implementation
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
 * @ingroup IO
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
#    include <io.h>
#endif // !_WIN32

#ifndef QB_IO_SYS_FILE_H_
#    define QB_IO_SYS_FILE_H_

namespace qb::io::sys {

/**
 * @class file
 * @brief Wrapper class for file system operations
 * 
 * This class provides a platform-independent interface for basic file operations
 * such as opening, reading, writing, and closing files. It uses native file
 * descriptors internally and works on both Windows and POSIX platforms.
 */
class QB_API file {
    int _handle; /**< Native file descriptor handle */

public:
    /**
     * @brief Default constructor
     * 
     * Creates a file object with an invalid file descriptor.
     */
    file() noexcept;
    
    /**
     * @brief Copy constructor
     */
    file(file const &) = default;
    
    /**
     * @brief Constructor from file descriptor
     * @param fd Existing file descriptor to wrap
     */
    explicit file(int fd) noexcept;
    
    /**
     * @brief Constructor that opens a file
     * @param fname Path to the file to open
     * @param flags File open flags (defaults to O_RDWR - read/write mode)
     */
    explicit file(std::string const &fname, int flags = O_RDWR) noexcept;

    /**
     * @brief Returns the native file descriptor
     * @return The internal file descriptor
     */
    [[nodiscard]] int native_handle() const noexcept;
    
    /**
     * @brief Checks if the file is open
     * @return true if the file is open, false otherwise
     */
    [[nodiscard]] bool is_open() const noexcept;
    
    /**
     * @brief Opens a file with the specified parameters
     * @param fname Path to the file to open
     * @param flags File open flags (defaults to O_RDWR - read/write mode)
     * @param mode File creation mode (defaults to 0644 - rw-r--r--)
     */
    void open(std::string const &fname, int flags = O_RDWR, int mode = 0644) noexcept;
    
    /**
     * @brief Opens a file using an existing file descriptor
     * @param fd File descriptor to use
     */
    void open(int fd) noexcept;
    
    /**
     * @brief Writes data to the file
     * @param data Pointer to the data to write
     * @param size Number of bytes to write
     * @return Number of bytes written, or negative value on error
     */
    int write(const char *data, std::size_t size) const noexcept;
    
    /**
     * @brief Reads data from the file
     * @param data Buffer to store the read data
     * @param size Maximum number of bytes to read
     * @return Number of bytes read, or negative value on error
     */
    int read(char *data, std::size_t size) const noexcept;
    
    /**
     * @brief Closes the file
     * 
     * This method closes the file descriptor and resets the handle to invalid.
     * If the file is already closed, this method does nothing.
     */
    void close() noexcept;

    /**
     * @brief Placeholder for setting non-blocking mode
     * 
     * This is currently unused and has no effect.
     * 
     * @param unused Unused parameter
     */
    void
    set_non_blocking(bool) const noexcept {}
};

/**
 * @class file_to_pipe
 * @brief Utility for reading file contents into a memory pipe
 * 
 * This class provides functionality for transferring data from a file
 * to a memory pipe, with tracking of read progress and file size.
 */
class QB_API file_to_pipe {
    qb::allocator::pipe<char> &_pipe;    /**< Reference to the destination pipe */
    file _handle;                         /**< File handle for reading */
    std::size_t _expected_size = 0;       /**< Total size of the file in bytes */
    std::size_t _read_bytes = 0;          /**< Number of bytes already read */

public:
    /**
     * @brief Default constructor is deleted
     */
    file_to_pipe() = delete;
    
    /**
     * @brief Destructor
     * 
     * Automatically closes the file if it's still open.
     */
    ~file_to_pipe() noexcept;
    
    /**
     * @brief Constructor
     * @param out Reference to the pipe that will receive the file data
     */
    file_to_pipe(qb::allocator::pipe<char> &out) noexcept;

    /**
     * @brief Opens a file for reading
     * @param path Path to the file to open
     * @return true if file was successfully opened, false otherwise
     * 
     * This method also retrieves the file size and resets read progress counters.
     */
    bool open(std::string const &path) noexcept;
    
    /**
     * @brief Reads data from the file into the pipe
     * @return Number of bytes read, or negative value on error
     * 
     * This method reads as much data as possible from the file to the pipe,
     * limited by the remaining unread bytes in the file.
     */
    [[nodiscard]] int read() noexcept;
    
    /**
     * @brief Reads all remaining data from the file
     * @return Last read operation result, or 0 if nothing was read
     * 
     * This method repeatedly calls read() until EOF is reached or an error occurs.
     */
    [[nodiscard]] int read_all() noexcept;
    
    /**
     * @brief Returns the number of bytes read so far
     * @return Count of bytes already read from the file
     */
    [[nodiscard]] std::size_t read_bytes() const noexcept;
    
    /**
     * @brief Returns the total file size
     * @return Total size of the file in bytes
     */
    [[nodiscard]] std::size_t expected_size() const noexcept;
    
    /**
     * @brief Checks if the file is open
     * @return true if the file is open, false otherwise
     */
    [[nodiscard]] bool is_open() const noexcept;
    
    /**
     * @brief Checks if all file data has been read
     * @return true if all data has been read, false otherwise
     */
    [[nodiscard]] bool eof() const noexcept;
    
    /**
     * @brief Closes the file
     */
    void close() noexcept;
};

/**
 * @class pipe_to_file
 * @brief Utility for writing memory pipe contents to a file
 * 
 * This class provides functionality for transferring data from a memory
 * pipe to a file, with tracking of write progress.
 */
class QB_API pipe_to_file {
    const qb::allocator::pipe<char> &_pipe;  /**< Reference to the source pipe */
    file _handle;                            /**< File handle for writing */
    std::size_t _written_bytes = 0;          /**< Number of bytes already written */

public:
    /**
     * @brief Default constructor is deleted
     */
    pipe_to_file() = delete;
    
    /**
     * @brief Destructor
     * 
     * Automatically closes the file if it's still open.
     */
    ~pipe_to_file() noexcept;
    
    /**
     * @brief Constructor
     * @param out Reference to the pipe containing the data to write
     */
    pipe_to_file(qb::allocator::pipe<char> const &out) noexcept;

    /**
     * @brief Opens a file for writing
     * @param path Path to the file to open
     * @param mode File creation mode (defaults to 0644 - rw-r--r--)
     * @return true if file was successfully opened, false otherwise
     * 
     * This method opens the file and resets the write progress counter.
     * If the file doesn't exist, it will be created.
     */
    bool open(std::string const &path, int mode = 0644) noexcept;
    
    /**
     * @brief Writes data from the pipe to the file
     * @return Number of bytes written, or negative value on error
     * 
     * This method writes a chunk of data from the pipe to the file,
     * starting from the current write position.
     */
    [[nodiscard]] int write() noexcept;
    
    /**
     * @brief Writes all remaining data from the pipe
     * @return Last write operation result, or negative value on error
     * 
     * This method repeatedly calls write() until all pipe contents have been written
     * or an error occurs.
     */
    [[nodiscard]] int write_all() noexcept;
    
    /**
     * @brief Returns the number of bytes written so far
     * @return Count of bytes already written to the file
     */
    [[nodiscard]] std::size_t written_bytes() const noexcept;
    
    /**
     * @brief Checks if the file is open
     * @return true if the file is open, false otherwise
     */
    [[nodiscard]] bool is_open() const noexcept;
    
    /**
     * @brief Checks if all pipe data has been written
     * @return true if all data has been written, false otherwise
     */
    [[nodiscard]] bool eos() const noexcept;
    
    /**
     * @brief Closes the file
     */
    void close() noexcept;
};

} // namespace qb::io::sys

#endif // QB_IO_SYS_FILE_H_
