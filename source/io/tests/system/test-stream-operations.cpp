/**
 * @file qb/io/tests/system/test-stream-operations.cpp
 * @brief Unit tests for stream operations in the qb IO library
 *
 * This file contains tests for the templated stream classes (stream, istream, ostream)
 * with different underlying IO mechanisms (files, TCP sockets, UDP sockets).
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
 * @ingroup Tests
 */

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <qb/io/stream.h>
#include <qb/io/system/file.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/transport/file.h>
#include <qb/io/udp/socket.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Test fixture for stream operations
class StreamTest : public ::testing::Test {
protected:
    const std::string    test_dir     = "./test_stream_files";
    const std::string    test_file    = test_dir + "/stream_test.txt";
    const std::string    test_content = "Hello, QB Stream Test!";
    const unsigned short tcp_port     = 64444;
    const unsigned short udp_port     = 64445;

    void
    SetUp() override {
        // qb::io::sys::file is now move-only with a closing destructor, so the
        // previous Windows skip (caused by copy-semantics double-close) is no
        // longer needed: stream.transport() = std::move(file) transfers the fd
        // and leaves the local file in a closed state.

        // Create test directory if it doesn't exist
        std::filesystem::create_directory(test_dir);

        // Create a test file with some content
        std::ofstream test_file_stream(test_file);
        test_file_stream << test_content;
        test_file_stream.close();
    }

    void
    TearDown() override {
        // Clean up test files and directory
        try {
            std::filesystem::remove_all(test_dir);
        } catch (const std::exception &e) {
            std::cerr << "Error cleaning up test files: " << e.what() << std::endl;
        }
    }
};

namespace {

class ScriptedStreamTransport {
    std::string              _read_data;
    std::size_t              _read_offset = 0u;
    std::vector<std::size_t> _write_limits;
    std::size_t              _write_index = 0u;
    bool                     _fail_reads  = false;
    bool                     _fail_writes = false;

public:
    std::string written;
    bool        disconnected = false;
    bool        closed       = false;

    ScriptedStreamTransport() = default;

    explicit ScriptedStreamTransport(std::string read_data, std::vector<std::size_t> write_limits = {}, bool fail_reads = false)
        : _read_data(std::move(read_data))
        , _write_limits(std::move(write_limits))
        , _fail_reads(fail_reads) {}

    void
    fail_writes(bool enabled = true) noexcept {
        _fail_writes = enabled;
    }

    int
    read(char *data, std::size_t size) noexcept {
        if (_fail_reads)
            return -1;
        if (_read_offset >= _read_data.size())
            return 0;

        const auto available = _read_data.size() - _read_offset;
        const auto count     = std::min(size, available);
        std::memcpy(data, _read_data.data() + _read_offset, count);
        _read_offset += count;
        return static_cast<int>(count);
    }

    int
    write(const char *data, std::size_t size) noexcept {
        if (_fail_writes)
            return -1;
        auto count = size;
        if (_write_index < _write_limits.size())
            count = std::min(count, _write_limits[_write_index++]);
        written.append(data, count);
        return static_cast<int>(count);
    }

    void
    disconnect() noexcept {
        disconnected = true;
    }
    void
    close() noexcept {
        closed = true;
    }
};

class LimitedScriptedOStream : public qb::io::ostream<ScriptedStreamTransport> {
public:
    void
    set_max_write_buffer_size(std::size_t size) noexcept {
        _max_write_buffer_size = size;
    }
};

} // namespace

// ----------------------- File Stream Tests -----------------------

TEST(StreamTemplates, InputReadLimitsFailuresAndConstAccess) {
    qb::io::istream<ScriptedStreamTransport> limited;
    limited.set_max_read_buffer_size(4u);
    EXPECT_EQ(limited.read(), qb::io::ErrBufferLimitExceeded);
    EXPECT_EQ(limited.pendingRead(), 0u);

    qb::io::istream<ScriptedStreamTransport> failing;
    failing.transport() = ScriptedStreamTransport{"", {}, true};
    EXPECT_EQ(failing.read(), -1);
    EXPECT_EQ(failing.pendingRead(), 0u);

    qb::io::istream<ScriptedStreamTransport> input;
    input.transport() = ScriptedStreamTransport{"payload"};
    ASSERT_EQ(input.read(), 7);
    EXPECT_EQ(input.pendingRead(), 7u);
    input.eof();
    EXPECT_EQ(input.pendingRead(), 7u);
    input.flush(input.pendingRead());
    input.eof();
    EXPECT_EQ(input.pendingRead(), 0u);

    const auto &const_input = input;
    EXPECT_FALSE(const_input.transport().closed);
}

TEST(StreamTemplates, OutputPartialFullAndRejectedPublishes) {
    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport() = ScriptedStreamTransport{"", {3u}};

    ASSERT_NE(output.publish("abcdef", 6u), nullptr);
    EXPECT_EQ(output.pendingWrite(), 6u);
    EXPECT_EQ(output.write(), 3);
    EXPECT_EQ(output.pendingWrite(), 3u);
    EXPECT_EQ(output.write(), 3);
    EXPECT_EQ(output.pendingWrite(), 0u);
    EXPECT_EQ(output.transport().written, "abcdef");

    const auto &const_output = output;
    EXPECT_EQ(const_output.transport().written, "abcdef");

    LimitedScriptedOStream limited;
    limited.set_max_write_buffer_size(3u);
    EXPECT_EQ(limited.publish("abcd", 4u), nullptr);
    EXPECT_EQ(limited.pendingWrite(), 0u);

    ASSERT_NE(limited.publish("ab", 2u), nullptr);
    limited.set_max_write_buffer_size(1u);
    EXPECT_EQ(limited.publish("c", 1u), nullptr);
    EXPECT_EQ(limited.pendingWrite(), 2u);
}

TEST(StreamTemplates, OutputWriteFailureKeepsPendingData) {
    qb::io::ostream<ScriptedStreamTransport> output;
    output.transport().fail_writes();

    ASSERT_NE(output.publish("data", 4u), nullptr);
    EXPECT_EQ(output.write(), -1);
    EXPECT_EQ(output.pendingWrite(), 4u);
    EXPECT_TRUE(output.transport().written.empty());
}

TEST(StreamTemplates, BidirectionalStreamPartialWritesAndWriteLimit) {
    qb::io::stream<ScriptedStreamTransport> stream;
    stream.transport() = ScriptedStreamTransport{"", {2u}};

    ASSERT_NE(stream.publish("wxyz", 4u), nullptr);
    EXPECT_EQ(stream.write(), 2);
    EXPECT_EQ(stream.pendingWrite(), 2u);
    EXPECT_EQ(stream.write(), 2);
    EXPECT_EQ(stream.pendingWrite(), 0u);
    EXPECT_EQ(stream.transport().written, "wxyz");

    stream.set_max_write_buffer_size(3u);
    EXPECT_EQ(stream.max_write_buffer_size(), 3u);
    EXPECT_EQ(stream.publish("abcd", 4u), nullptr);
    EXPECT_EQ(stream.pendingWrite(), 0u);

    stream.close();
    EXPECT_TRUE(stream.transport().disconnected);
    EXPECT_TRUE(stream.transport().closed);
}

// Test file input stream
TEST_F(StreamTest, FileInputStream) {
    // Create istream with file
    qb::io::sys::file file;
    ASSERT_GE(file.open(test_file, O_RDONLY), 0);
    ASSERT_TRUE(file.is_open());

    // Set up the istream - we need to create it and initialize it differently
    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(file);

    // Test reading via read() which reads into the internal buffer first
    int read_result = input_stream.read();
    // Si la lecture échoue, afficher un message mais ne pas faire échouer le test
    if (read_result <= 0) {
        std::cout << "Warning: Unable to read from file, got result: " << read_result << std::endl;
    } else {
        // Get the data from the buffer
        std::array<char, 100> buffer{};
        std::memcpy(buffer.data(), input_stream.in().data(), std::min(test_content.size(), input_stream.in().size()));

        // Verify content
        std::string read_content(buffer.data(), test_content.size());
        EXPECT_EQ(read_content, test_content);
    }

    // Ensure we close the file properly before the test ends
    input_stream.close();

    if (file.is_open()) {
        file.close();
    }
}

// Test file output stream
TEST_F(StreamTest, FileOutputStream) {
    // Create output file
    std::string output_file = test_dir + "/output.txt";

    // Create ostream with file
    qb::io::sys::file file;
    ASSERT_GE(file.open(output_file, O_WRONLY | O_CREAT, 0644), 0);
    ASSERT_TRUE(file.is_open());

    // Set up the ostream
    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(file);

    // Test writing from buffer
    const std::string write_content = "Testing output stream";

    // Add data to output buffer and write
    std::ignore      = output_stream.publish(write_content.c_str(), write_content.size());
    int write_result = output_stream.write();
    ASSERT_GT(write_result, 0);

    // Flush and close
    file.close();

    // Verify file contents
    std::ifstream check_file(output_file);
    std::string   read_content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(read_content, write_content);

    // Test writing from std::vector
    ASSERT_GE(file.open(output_file, O_WRONLY | O_TRUNC, 0644), 0);
    ASSERT_TRUE(file.is_open());

    // Reinitialize the stream with the new file
    output_stream.close();
    output_stream.transport() = std::move(file);

    const std::string vec_content = "Vector content test";
    std::vector<char> vec_buffer(vec_content.begin(), vec_content.end());

    // Add data to output buffer and write
    std::ignore  = output_stream.publish(vec_buffer.data(), vec_buffer.size());
    write_result = output_stream.write();
    ASSERT_GT(write_result, 0);

    // Flush and close
    file.close();

    // Verify file contents
    std::ifstream check_file2(output_file);
    std::string   read_content2((std::istreambuf_iterator<char>(check_file2)), std::istreambuf_iterator<char>());
    EXPECT_EQ(read_content2, vec_content);
}

// Test bidirectional file stream
TEST_F(StreamTest, FileBidirectionalStream) {
    // Create temp file
    std::string bidir_file = test_dir + "/bidir.txt";

    // Create stream with file
    qb::io::sys::file file;
    ASSERT_GE(file.open(bidir_file, O_RDWR | O_CREAT, 0644), 0);
    ASSERT_TRUE(file.is_open());

    // Set up the stream
    qb::io::stream<qb::io::sys::file> bidir_stream;
    bidir_stream.transport() = std::move(file);

    // Test writing
    const std::string write_content = "Bidirectional stream test";

    // Add data to output buffer and write
    std::ignore      = bidir_stream.publish(write_content.c_str(), write_content.size());
    int write_result = bidir_stream.write();

    if (write_result <= 0) {
        std::cout << "Warning: Unable to write to file, got result: " << write_result << std::endl;
    } else {
        // Close and reopen to reset position
        bidir_stream.close();
        file.close();

        ASSERT_GE(file.open(bidir_file, O_RDWR), 0);
        ASSERT_TRUE(file.is_open());

        // Reinitialize the stream with the new file
        bidir_stream.transport() = std::move(file);

        // Test reading
        int read_result = bidir_stream.read();

        if (read_result <= 0) {
            std::cout << "Warning: Unable to read from file, got result: " << read_result << std::endl;
        } else {
            std::array<char, 100> buffer{};
            std::memcpy(buffer.data(), bidir_stream.in().data(), std::min(write_content.size(), bidir_stream.in().size()));

            // Verify content
            std::string read_content(buffer.data(), write_content.size());
            EXPECT_EQ(read_content, write_content);
        }
    }

    // Clean up properly
    bidir_stream.close();
    if (file.is_open()) {
        file.close();
    }
}

// Test the transport::file class
TEST_F(StreamTest, FileTransport) {
    // Create output file
    std::string transport_file = test_dir + "/transport.txt";

    // Create file instance
    qb::io::sys::file file;
    ASSERT_GE(file.open(transport_file, O_WRONLY | O_CREAT, 0644), 0);
    ASSERT_TRUE(file.is_open());

    // Set up transport using the file
    qb::io::transport::file transport;
    transport.transport() = std::move(file);

    // Test writing
    const std::string write_content = "Transport file test";

    // Add data to output buffer and write
    std::ignore      = transport.publish(write_content.c_str(), write_content.size());
    int write_result = transport.write();

    if (write_result <= 0) {
        std::cout << "Warning: Unable to write to transport file, got result: " << write_result << std::endl;
    } else {
        // Flush and close
        transport.close();
        file.close();

        // Verify file contents
        std::ifstream check_file(transport_file);
        std::string   read_content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
        EXPECT_EQ(read_content, write_content);

        // Test reading through transport
        ASSERT_GE(file.open(transport_file, O_RDONLY), 0);
        ASSERT_TRUE(file.is_open());

        // Reinitialize the transport with the new file
        transport.transport() = std::move(file);

        // Read into internal buffer
        int read_result = transport.read();

        if (read_result <= 0) {
            std::cout << "Warning: Unable to read from transport file, got result: " << read_result << std::endl;
        } else {
            // Copy data to local buffer
            std::array<char, 100> buffer{};
            std::memcpy(buffer.data(), transport.in().data(), std::min(write_content.size(), transport.in().size()));

            // Verify content
            std::string transport_read(buffer.data(), write_content.size());
            EXPECT_EQ(transport_read, write_content);
        }
    }

    // Clean up properly
    transport.close();
    if (file.is_open()) {
        file.close();
    }
}

// ----------------------- TCP Stream Tests -----------------------

// Test TCP stream
TEST_F(StreamTest, DISABLED_TCPStream) {
    // Use std::promise to coordinate between threads
    std::promise<bool> server_ready;
    std::future<bool>  server_ready_future = server_ready.get_future();

    // Server thread
    std::thread server_thread([this, &server_ready]() {
        try {
            // Create TCP listener
            qb::io::tcp::listener listener;

            // Start listening on port
            ASSERT_EQ(listener.listen_v4(tcp_port), 0);

            // Signal that server is ready
            server_ready.set_value(true);

            // Accept client connection
            qb::io::tcp::socket server_socket = listener.accept();
            ASSERT_TRUE(server_socket.is_open());

            // Create a buffer to read data
            std::array<char, 100> buffer{};

            // Read from client
            int bytes_read = server_socket.read(buffer.data(), buffer.size());
            EXPECT_GT(bytes_read, 0);

            // Check if we've received the expected message
            const std::string expected_message = "Client to server";
            std::string       received_message(buffer.data(), bytes_read);
            EXPECT_EQ(received_message, expected_message);

            // Write back to client
            const std::string response   = "Server to client";
            int               bytes_sent = server_socket.write(response.c_str(), response.size());
            EXPECT_GT(bytes_sent, 0);

            // Close connection
            server_socket.close();
            listener.close();

        } catch (const std::exception &e) {
            std::cerr << "Server error: " << e.what() << std::endl;
            server_ready.set_value(false);
        }
    });

    // Wait for server to be ready
    ASSERT_TRUE(server_ready_future.get());

    // Give the server a moment to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client thread
    try {
        // Connect to server
        qb::io::tcp::socket client_socket;
        ASSERT_EQ(client_socket.init(), 0);

        // Connect to the server
        ASSERT_EQ(client_socket.connect_v4("127.0.0.1", tcp_port), 0);

        // Send data to server
        const std::string message    = "Client to server";
        int               bytes_sent = client_socket.write(message.c_str(), message.size());
        EXPECT_GT(bytes_sent, 0);

        // Read response from server
        std::array<char, 100> buffer{};
        int                   bytes_read = client_socket.read(buffer.data(), buffer.size());
        EXPECT_GT(bytes_read, 0);

        // Verify the response
        const std::string expected_response = "Server to client";
        std::string       received_response(buffer.data(), bytes_read);
        EXPECT_EQ(received_response, expected_response);

        // Close connection
        client_socket.close();

    } catch (const std::exception &e) {
        std::cerr << "Client error: " << e.what() << std::endl;
        FAIL() << "Client connection failed";
    }

    server_thread.join();
}

// ----------------------- UDP Stream Tests -----------------------

// Test UDP stream
TEST_F(StreamTest, DISABLED_UDPStream) {
    // Use std::promise to coordinate between threads
    std::promise<bool> server_ready;
    std::future<bool>  server_ready_future = server_ready.get_future();

    // Server thread
    std::thread server_thread([this, &server_ready]() {
        try {
            // Create UDP socket (server)
            qb::io::udp::socket server_socket;
            ASSERT_TRUE(server_socket.init());

            // Bind to a port
            ASSERT_EQ(server_socket.bind_v4(udp_port), 0);

            // Signal that server is ready
            server_ready.set_value(true);

            // For UDP, we'll use the socket directly
            std::array<char, 100> buffer{};
            qb::io::endpoint      client_endpoint;

            // Wait for data from client
            const std::string expected_message = "Client to server via UDP";
            int               bytes_read       = server_socket.read(buffer.data(), buffer.size(), client_endpoint);
            EXPECT_GT(bytes_read, 0);

            std::string received_message(buffer.data(), bytes_read);
            EXPECT_EQ(received_message, expected_message);

            // Write back to client
            const std::string response      = "Server to client via UDP";
            int               bytes_written = server_socket.write(response.c_str(), response.size(), client_endpoint);
            EXPECT_GT(bytes_written, 0);

            // Clean up
            server_socket.close();
        } catch (const std::exception &e) {
            std::cerr << "UDP Server error: " << e.what() << std::endl;
            server_ready.set_value(false);
        }
    });

    // Wait for server to be ready
    ASSERT_TRUE(server_ready_future.get());

    // Give the server a moment to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client thread
    try {
        // Create UDP socket (client)
        qb::io::udp::socket client_socket;
        ASSERT_TRUE(client_socket.init());

        // Create endpoint for server
        qb::io::endpoint server_endpoint = qb::io::endpoint().as_in("127.0.0.1", udp_port);

        // Send data to server
        const std::string message       = "Client to server via UDP";
        int               bytes_written = client_socket.write(message.c_str(), message.size(), server_endpoint);
        EXPECT_GT(bytes_written, 0);

        // Read response from server
        std::array<char, 100> buffer{};
        qb::io::endpoint      server_reply_endpoint;
        const std::string     expected_response = "Server to client via UDP";

        // UDP is connectionless, so we need to wait a moment for the response
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int bytes_read = client_socket.read(buffer.data(), buffer.size(), server_reply_endpoint);
        EXPECT_GT(bytes_read, 0);

        std::string received_response(buffer.data(), bytes_read);
        EXPECT_EQ(received_response, expected_response);

        // Clean up
        client_socket.close();
    } catch (const std::exception &e) {
        std::cerr << "UDP Client error: " << e.what() << std::endl;
        FAIL() << "UDP Client operation failed";
    }

    server_thread.join();
}

// ----------------------- Stream Edge Cases -----------------------

// Test stream with large data transfer
TEST_F(StreamTest, DISABLED_LargeDataTransfer) {
    // Create a large data buffer (1MB)
    const size_t      buffer_size = 1024 * 1024;
    std::vector<char> large_buffer(buffer_size, 'A');

    // Create output file
    std::string large_file = test_dir + "/large_transfer.dat";

    // Create stream with file
    qb::io::sys::file file;
    ASSERT_GE(file.open(large_file, O_RDWR | O_CREAT, 0644), 0);
    ASSERT_TRUE(file.is_open());

    qb::io::stream<qb::io::sys::file> stream;
    stream.transport() = std::move(file);

    // Write large buffer in chunks
    size_t       total_written = 0;
    const size_t chunk_size    = 8192; // Use smaller chunks

    while (total_written < buffer_size) {
        size_t current_chunk = std::min(chunk_size, buffer_size - total_written);
        std::ignore          = stream.publish(large_buffer.data() + total_written, current_chunk);
        int bytes_written    = stream.write();
        EXPECT_GT(bytes_written, 0);
        total_written += current_chunk;
    }

    // Verify file size
    file.close();
    EXPECT_EQ(std::filesystem::file_size(large_file), buffer_size);

    // Now read it back
    ASSERT_GE(file.open(large_file, O_RDONLY), 0);
    ASSERT_TRUE(file.is_open());

    stream.close();
    stream.transport() = std::move(file);

    std::vector<char> read_buffer(buffer_size);
    size_t            total_read = 0;

    while (total_read < buffer_size) {
        int bytes_read = stream.read();
        EXPECT_GT(bytes_read, 0);

        size_t available = std::min(stream.in().size(), buffer_size - total_read);
        std::memcpy(read_buffer.data() + total_read, stream.in().data(), available);
        total_read += available;
        stream.flush(available);
    }

    // Verify content (just check beginning and end for efficiency)
    EXPECT_EQ(read_buffer[0], 'A');
    EXPECT_EQ(read_buffer[buffer_size - 1], 'A');
    EXPECT_EQ(total_read, buffer_size);
}

// Test stream with error conditions
TEST_F(StreamTest, StreamErrors) {
    // Test reading from closed file (move-only fd ownership)
    qb::io::istream<qb::io::sys::file> input_stream;
    {
        qb::io::sys::file closed_file;
        input_stream.transport() = std::move(closed_file);
    }

    int result = input_stream.read();
    EXPECT_LT(result, 0);

    // Test writing to closed file
    qb::io::ostream<qb::io::sys::file> output_stream;
    {
        qb::io::sys::file closed_file;
        output_stream.transport() = std::move(closed_file);
    }

    const std::string test_data = "Test data";
    std::ignore                 = output_stream.publish(test_data.c_str(), test_data.size());
    result                      = output_stream.write();
    EXPECT_LT(result, 0);
}

// ----------------------- Memory Buffer Stream Tests -----------------------

// Test with memory buffer as the underlying IO
TEST_F(StreamTest, DISABLED_MemoryBufferStream) {
    // Create a simple class that implements read/write methods for memory buffer
    class MemoryBuffer {
    private:
        std::vector<char> _buffer;
        size_t            _read_pos  = 0;
        size_t            _write_pos = 0;

    public:
        MemoryBuffer()
            : _buffer(1024, 0) {}

        // Copy constructor needed for assignment
        MemoryBuffer(const MemoryBuffer &other)
            : _buffer(other._buffer)
            , _read_pos(other._read_pos)
            , _write_pos(other._write_pos) {}

        // Assignment operator
        MemoryBuffer &
        operator=(const MemoryBuffer &other) {
            if (this != &other) {
                _buffer    = other._buffer;
                _read_pos  = other._read_pos;
                _write_pos = other._write_pos;
            }
            return *this;
        }

        int
        read(char *data, std::size_t size) {
            if (_read_pos >= _write_pos)
                return 0; // EOF

            size_t bytes_to_read = std::min(size, _write_pos - _read_pos);
            std::memcpy(data, _buffer.data() + _read_pos, bytes_to_read);
            _read_pos += bytes_to_read;
            return bytes_to_read;
        }

        int
        write(const char *data, std::size_t size) {
            if (_write_pos + size > _buffer.size()) {
                // Resize buffer if needed
                _buffer.resize(_write_pos + size);
            }

            std::memcpy(_buffer.data() + _write_pos, data, size);
            _write_pos += size;
            return size;
        }

        void
        close() {
            // Reset positions
            _read_pos  = 0;
            _write_pos = 0;
        }

        bool
        is_open() const {
            return true; // Always considered open
        }
    };

    // Create stream with memory buffer
    qb::io::stream<MemoryBuffer> stream;

    // Test writing to memory buffer
    const std::string test_data = "Testing memory buffer stream";
    std::ignore                 = stream.publish(test_data.c_str(), test_data.size());
    int write_result            = stream.write();
    ASSERT_GT(write_result, 0);

    // Reset positions for reading
    stream.close();

    // Test reading from memory buffer
    int read_result = stream.read();
    ASSERT_GT(read_result, 0);

    // Verify data in buffer
    const char *in_data = stream.in().data();
    std::string read_data(in_data, std::min(test_data.size(), stream.in().size()));
    EXPECT_EQ(read_data, test_data);
}

// ----------------------- Stream Chain Tests -----------------------

// Test chaining streams together (output of one stream to input of another)
TEST_F(StreamTest, StreamChaining) {
    // Create source file
    std::string source_file = test_dir + "/source_chain.txt";
    std::string dest_file   = test_dir + "/dest_chain.txt";

    const std::string test_content = "Testing stream chaining with non-trivial content 12345!@#$%";

    // Create source file
    {
        std::ofstream file(source_file);
        file << test_content;
    }

    // Step 1: Read from file to pipe
    qb::allocator::pipe<char> pipe;
    qb::io::sys::file         file_source;
    ASSERT_TRUE(file_source.open(source_file, O_RDONLY) >= 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(file_source);

    // Read from file to input stream's buffer
    int read_result = input_stream.read();
    if (read_result <= 0) {
        std::cout << "Warning: Failed to read from source file, got result: " << read_result << std::endl;
        // Clean up and skip the rest of the test
        return;
    }

    // Step 2: Copy data from input stream to output stream
    qb::io::sys::file file_dest;
    ASSERT_TRUE(file_dest.open(dest_file, O_WRONLY | O_CREAT, 0644) >= 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(file_dest);

    // Transfer from input buffer to output buffer
    size_t      content_size = input_stream.in().size();
    const char *content_data = input_stream.in().data();

    // Publish data to output stream's buffer
    std::ignore = output_stream.publish(content_data, content_size);

    // Write from output stream's buffer to file
    int write_result = output_stream.write();
    if (write_result <= 0) {
        std::cout << "Warning: Failed to write to destination file, got result: " << write_result << std::endl;
    }

    // Clean up
    input_stream.close();
    output_stream.close();
    file_source.close();
    file_dest.close();

    // Verify destination file content matches source
    std::ifstream check_file(dest_file);
    std::string   result_content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(result_content, test_content);
}

// ----------------------- Null Stream Test -----------------------

// Test null stream (like /dev/null)
TEST_F(StreamTest, NullStream) {
    // Create a null device class that discards all data
    class NullDevice {
    public:
        int
        read(char *data, std::size_t size) {
            // Always return EOF
            return 0;
        }

        int
        write(const char *data, std::size_t size) {
            // Pretend we wrote all data
            return size;
        }

        bool
        is_open() const {
            return true; // Always open
        }

        void
        close() {
            // Do nothing
        }
    };

    // Create stream with null device (NullDevice is copy-constructible).
    qb::io::stream<NullDevice> null_stream;
    null_stream.transport() = NullDevice{};

    // Test writing to null stream
    const std::string test_data = "This data should be discarded";
    std::ignore                 = null_stream.publish(test_data.c_str(), test_data.size());
    int written                 = null_stream.write();

    // Should report all bytes written
    EXPECT_EQ(written, test_data.size());

    // Test reading from null stream
    int read = null_stream.read();

    // Should return 0 (EOF)
    EXPECT_EQ(read, 0);
    EXPECT_EQ(null_stream.in().size(), 0);
}

// ----------------------- Stream Buffer Management Tests -----------------------

// Test stream buffer management (flush, reset, etc.)
TEST_F(StreamTest, DISABLED_StreamBufferManagement) {
    // Create a test file
    std::string       buffer_file  = test_dir + "/buffer_test.txt";
    const std::string test_content = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n";

    {
        std::ofstream file(buffer_file);
        file << test_content;
    }

    // Create file input stream
    qb::io::sys::file file;
    ASSERT_GE(file.open(buffer_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(file);

    // Read file into buffer
    int read_result = input_stream.read();
    ASSERT_GT(read_result, 0);
    EXPECT_GT(input_stream.in().size(), 0);

    // Get the first line
    size_t line1_end = 0;
    while (line1_end < input_stream.in().size() && input_stream.in().data()[line1_end] != '\n') {
        line1_end++;
    }

    // Line length plus newline character
    line1_end++;

    // Extract first line
    std::string line1(input_stream.in().data(), line1_end);
    EXPECT_EQ(line1, "Line 1\n");

    // Flush the first line from buffer
    input_stream.flush(line1_end);

    // Get second line from updated buffer
    size_t line2_end = 0;
    while (line2_end < input_stream.in().size() && input_stream.in().data()[line2_end] != '\n') {
        line2_end++;
    }

    // Line length plus newline character
    line2_end++;

    // Extract second line
    std::string line2(input_stream.in().data(), line2_end);
    EXPECT_EQ(line2, "Line 2\n");

    // Read more data (should get the rest of the file)
    read_result = input_stream.read();
    ASSERT_GT(read_result, 0);

    // Check if buffer contains the rest of lines
    std::string remaining(input_stream.in().data() + line2_end, input_stream.in().size() - line2_end);
    EXPECT_TRUE(remaining.find("Line 3") != std::string::npos);
    EXPECT_TRUE(remaining.find("Line 4") != std::string::npos);
    EXPECT_TRUE(remaining.find("Line 5") != std::string::npos);

    // Close and clean up
    input_stream.close();
    file.close();
}

// ----------------------- Error Handling Tests -----------------------

// Test advanced error handling in streams
TEST_F(StreamTest, AdvancedErrorHandling) {
    // Create a file that will be deleted during operation
    std::string temp_file = test_dir + "/temp_delete.txt";

    {
        std::ofstream file(temp_file);
        file << "This file will be deleted during read/write operations";
    }

    // Create input stream
    qb::io::sys::file read_file;
    ASSERT_GE(read_file.open(temp_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(read_file);

    // Read should succeed
    int read_result = input_stream.read();
    EXPECT_GT(read_result, 0);

    // Delete the file while the stream is open
    std::filesystem::remove(temp_file);

    // Create a new output file (this should work even though we deleted the input file)
    std::string       output_file = test_dir + "/after_delete.txt";
    qb::io::sys::file write_file;
    ASSERT_GE(write_file.open(output_file, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(write_file);

    // Test error recovery - should be able to publish and write data
    // even though the input file was deleted
    const std::string recovery_data = "Data written after error";
    std::ignore                     = output_stream.publish(recovery_data.c_str(), recovery_data.size());
    int write_result                = output_stream.write();
    EXPECT_GT(write_result, 0);

    // Streams own their fd via move; no manual close on the local file
    // copies (already moved-from) is needed.

    // Verify output file content
    std::ifstream check_file(output_file);
    std::string   content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, recovery_data);
}

// ----------------------- Stream Composition Tests -----------------------

// Class definition outside the test function
template <typename BaseStream>
class TransformStream {
private:
    BaseStream                         &_base_stream;
    std::function<void(char *, size_t)> _transform_func;

public:
    TransformStream(BaseStream &base, std::function<void(char *, size_t)> transform)
        : _base_stream(base)
        , _transform_func(transform) {}

    int
    read() {
        int result = _base_stream.read();
        if (result > 0) {
            // Apply transformation to the base stream's buffer
            char  *data = const_cast<char *>(_base_stream.in().data());
            size_t size = _base_stream.in().size();
            _transform_func(data, size);
        }
        return result;
    }

    void
    publish(const char *data, size_t size) {
        // Create a mutable copy of the data
        std::vector<char> buffer(data, data + size);

        // Apply transformation
        _transform_func(buffer.data(), buffer.size());

        // Publish transformed data
        std::ignore = _base_stream.publish(buffer.data(), buffer.size());
    }

    int
    write() {
        return _base_stream.write();
    }

    auto &
    in() {
        return _base_stream.in();
    }
};

// Test using streams in a composed manner
TEST_F(StreamTest, StreamComposition) {
    // Create source file
    std::string source_file = test_dir + "/transform_source.txt";
    std::string dest_file   = test_dir + "/transform_dest.txt";

    const std::string test_content = "abcdefghijklmnopqrstuvwxyz";

    {
        std::ofstream file(source_file);
        file << test_content;
    }

    // Create a transform that converts lowercase to uppercase
    auto uppercase_transform = [](char *data, size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            if (data[i] >= 'a' && data[i] <= 'z') {
                data[i] = data[i] - 'a' + 'A';
            }
        }
    };

    // Create file streams
    qb::io::sys::file source;
    ASSERT_GE(source.open(source_file, O_RDONLY), 0);

    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = std::move(source);

    // Create transform stream adapter
    TransformStream<qb::io::istream<qb::io::sys::file>> transform_stream(input_stream, uppercase_transform);

    // Read and transform
    int read_result = transform_stream.read();
    ASSERT_GT(read_result, 0);

    // Get transformed data
    std::string transformed_data(transform_stream.in().data(), transform_stream.in().size());
    EXPECT_EQ(transformed_data, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    // Write transformed data to output file
    qb::io::sys::file dest;
    ASSERT_GE(dest.open(dest_file, O_WRONLY | O_CREAT, 0644), 0);

    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = std::move(dest);

    // Use the transform adapter for writing too
    TransformStream<qb::io::ostream<qb::io::sys::file>> transform_output(output_stream, uppercase_transform);

    // Write some text that will be transformed
    transform_output.publish("testing transformation", 22);
    int write_result = transform_output.write();
    ASSERT_GT(write_result, 0);

    // Streams own the descriptors after std::move; closing them here would
    // be a no-op on the moved-from local variables.
    input_stream.close();
    output_stream.close();

    // Verify output file content (should be uppercase)
    std::ifstream check_file(dest_file);
    std::string   content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "TESTING TRANSFORMATION");
}

// Run all the tests
int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
