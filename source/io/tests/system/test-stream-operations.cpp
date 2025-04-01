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

#include <gtest/gtest.h>
#include <qb/io/stream.h>
#include <qb/io/system/file.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/udp/socket.h>
#include <qb/io/transport/file.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <future>
#include <string>
#include <vector>
#include <array>

// Test fixture for stream operations
class StreamTest : public ::testing::Test {
protected:
    const std::string test_dir = "./test_stream_files";
    const std::string test_file = test_dir + "/stream_test.txt";
    const std::string test_content = "Hello, QB Stream Test!";
    const unsigned short tcp_port = 64444;
    const unsigned short udp_port = 64445;

    void SetUp() override {
        // Create test directory if it doesn't exist
        std::filesystem::create_directory(test_dir);
        
        // Create a test file with some content
        std::ofstream test_file_stream(test_file);
        test_file_stream << test_content;
        test_file_stream.close();
    }

    void TearDown() override {
        // Clean up test files and directory
        try {
            std::filesystem::remove_all(test_dir);
        } catch (const std::exception& e) {
            std::cerr << "Error cleaning up test files: " << e.what() << std::endl;
        }
    }
};

// ----------------------- File Stream Tests -----------------------

// Test file input stream
TEST_F(StreamTest, FileInputStream) {
    // Create istream with file
    qb::io::sys::file file;
    ASSERT_TRUE(file.open(test_file, O_RDONLY) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Set up the istream - we need to create it and initialize it differently
    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = file;
    
    // Test reading via read() which reads into the internal buffer first
    input_stream.read();
    
    // Get the data from the buffer
    std::array<char, 100> buffer{};
    std::memcpy(buffer.data(), input_stream.in().data(), std::min(test_content.size(), input_stream.in().size()));
    
    // Verify content
    std::string read_content(buffer.data(), test_content.size());
    EXPECT_EQ(read_content, test_content);
    
    // Test reading into std::vector
    std::vector<char> vec_buffer(100);
    
    // Reopen the file as we've already read everything
    file.close();
    ASSERT_TRUE(file.open(test_file, O_RDONLY) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Reinitialize the stream with the new file
    input_stream.close();
    input_stream.transport() = file;
    
    // Read into buffer and copy to vector
    input_stream.read();
    std::memcpy(vec_buffer.data(), input_stream.in().data(), std::min(test_content.size(), input_stream.in().size()));
    
    // Verify content
    std::string vec_read_content(vec_buffer.data(), test_content.size());
    EXPECT_EQ(vec_read_content, test_content);
}

// Test file output stream
TEST_F(StreamTest, FileOutputStream) {
    // Create output file
    std::string output_file = test_dir + "/output.txt";
    
    // Create ostream with file
    qb::io::sys::file file;
    ASSERT_TRUE(file.open(output_file, O_WRONLY | O_CREAT, 0644) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Set up the ostream
    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = file;
    
    // Test writing from buffer
    const std::string write_content = "Testing output stream";
    
    // Add data to output buffer and write
    output_stream.publish(write_content.c_str(), write_content.size());
    output_stream.write();
    
    // Flush and close
    file.close();
    
    // Verify file contents
    std::ifstream check_file(output_file);
    std::string read_content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(read_content, write_content);
    
    // Test writing from std::vector
    ASSERT_TRUE(file.open(output_file, O_WRONLY | O_TRUNC, 0644) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Reinitialize the stream with the new file
    output_stream.close();
    output_stream.transport() = file;
    
    const std::string vec_content = "Vector content test";
    std::vector<char> vec_buffer(vec_content.begin(), vec_content.end());
    
    // Add data to output buffer and write
    output_stream.publish(vec_buffer.data(), vec_buffer.size());
    output_stream.write();
    
    // Flush and close
    file.close();
    
    // Verify file contents
    std::ifstream check_file2(output_file);
    std::string read_content2((std::istreambuf_iterator<char>(check_file2)), std::istreambuf_iterator<char>());
    EXPECT_EQ(read_content2, vec_content);
}

// Test bidirectional file stream
TEST_F(StreamTest, FileBidirectionalStream) {
    // Create temp file
    std::string bidir_file = test_dir + "/bidir.txt";
    
    // Create stream with file
    qb::io::sys::file file;
    ASSERT_TRUE(file.open(bidir_file, O_RDWR | O_CREAT, 0644) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Set up the stream
    qb::io::stream<qb::io::sys::file> bidir_stream;
    bidir_stream.transport() = file;
    
    // Test writing
    const std::string write_content = "Bidirectional stream test";
    
    // Add data to output buffer and write
    bidir_stream.publish(write_content.c_str(), write_content.size());
    bidir_stream.write();
    
    // Close and reopen to reset position
    file.close();
    ASSERT_TRUE(file.open(bidir_file, O_RDWR) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Reinitialize the stream with the new file
    bidir_stream.close();
    bidir_stream.transport() = file;
    
    // Test reading
    bidir_stream.read();
    std::array<char, 100> buffer{};
    std::memcpy(buffer.data(), bidir_stream.in().data(), std::min(write_content.size(), bidir_stream.in().size()));
    
    // Verify content
    std::string read_content(buffer.data(), write_content.size());
    EXPECT_EQ(read_content, write_content);
}

// Test the transport::file class
TEST_F(StreamTest, FileTransport) {
    // Create output file
    std::string transport_file = test_dir + "/transport.txt";
    
    // Create file instance
    qb::io::sys::file file;
    ASSERT_TRUE(file.open(transport_file, O_WRONLY | O_CREAT, 0644) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Set up transport using the file
    qb::io::transport::file transport;
    transport.transport() = file;
    
    // Test writing
    const std::string write_content = "Transport file test";
    
    // Add data to output buffer and write
    transport.publish(write_content.c_str(), write_content.size());
    transport.write();
    
    // Flush and close
    file.close();
    
    // Verify file contents
    std::ifstream check_file(transport_file);
    std::string read_content((std::istreambuf_iterator<char>(check_file)), std::istreambuf_iterator<char>());
    EXPECT_EQ(read_content, write_content);
    
    // Test reading through transport
    ASSERT_TRUE(file.open(transport_file, O_RDONLY) >= 0);
    ASSERT_TRUE(file.is_open());
    
    // Reinitialize the transport with the new file
    transport.close();
    transport.transport() = file;
    
    // Read into internal buffer
    transport.read();
    
    // Copy data to local buffer
    std::array<char, 100> buffer{};
    std::memcpy(buffer.data(), transport.in().data(), std::min(write_content.size(), transport.in().size()));
    
    // Verify content
    std::string transport_read(buffer.data(), write_content.size());
    EXPECT_EQ(transport_read, write_content);
}

// ----------------------- TCP Stream Tests -----------------------

// Test TCP stream
TEST_F(StreamTest, TCPStream) {
    // Use std::promise to coordinate between threads
    std::promise<bool> server_ready;
    std::future<bool> server_ready_future = server_ready.get_future();
    
    // Server thread
    std::thread server_thread([this, &server_ready]() {
        try {
            // Create TCP listener
            qb::io::tcp::listener listener;
            EXPECT_EQ(listener.listen_v4(tcp_port), qb::io::SocketStatus::Done);
            
            // Signal that server is ready
            server_ready.set_value(true);
            
            // Accept client connection
            qb::io::tcp::socket server_socket;
            EXPECT_EQ(listener.accept(server_socket), qb::io::SocketStatus::Done);
            
            // Create stream for server socket
            qb::io::stream<qb::io::tcp::socket> server_stream;
            server_stream.transport() = server_socket;
            
            // Read from client - need to read into internal buffer first
            server_stream.read();
            
            // Check if we've received the expected message
            const std::string expected_message = "Client to server";
            std::string received_message(server_stream.in().data(), std::min(expected_message.size(), server_stream.in().size()));
            EXPECT_EQ(received_message, expected_message);
            
            // Write back to client
            const std::string response = "Server to client";
            server_stream.publish(response.c_str(), response.size());
            server_stream.write();
            
            // Close connection
            server_socket.disconnect();
        } catch (const std::exception& e) {
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
        EXPECT_EQ(client_socket.connect_v4("127.0.0.1", tcp_port), qb::io::SocketStatus::Done);
        
        // Create stream for client socket
        qb::io::stream<qb::io::tcp::socket> client_stream;
        client_stream.transport() = client_socket;
        
        // Write to server
        const std::string message = "Client to server";
        client_stream.publish(message.c_str(), message.size());
        client_stream.write();
        
        // Read response from server
        client_stream.read();
        
        // Verify the response
        const std::string expected_response = "Server to client";
        std::string received_response(client_stream.in().data(), std::min(expected_response.size(), client_stream.in().size()));
        EXPECT_EQ(received_response, expected_response);
        
        // Close connection
        client_socket.disconnect();
    } catch (const std::exception& e) {
        std::cerr << "Client error: " << e.what() << std::endl;
        FAIL() << "Client connection failed";
    }
    
    server_thread.join();
}

// ----------------------- UDP Stream Tests -----------------------

// Test UDP stream
TEST_F(StreamTest, UDPStream) {
    // Use std::promise to coordinate between threads
    std::promise<bool> server_ready;
    std::future<bool> server_ready_future = server_ready.get_future();
    
    // Server thread
    std::thread server_thread([this, &server_ready]() {
        try {
            // Create UDP socket (server)
            qb::io::udp::socket server_socket;
            EXPECT_EQ(server_socket.bind_v4(udp_port), qb::io::SocketStatus::Done);
            
            // Signal that server is ready
            server_ready.set_value(true);
            
            // For UDP, we'll use the socket directly since streams don't handle UDP's connectionless nature well
            std::array<char, 100> buffer{};
            qb::io::endpoint client_endpoint;
            
            const std::string expected_message = "Client to server via UDP";
            int bytes_read = server_socket.read(buffer.data(), buffer.size(), client_endpoint);
            EXPECT_GT(bytes_read, 0);
            
            std::string received_message(buffer.data(), bytes_read);
            EXPECT_EQ(received_message, expected_message);
            
            // Write back to client
            const std::string response = "Server to client via UDP";
            server_socket.write(response.c_str(), response.size(), client_endpoint);
            
            // Clean up
            server_socket.close();
        } catch (const std::exception& e) {
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
        EXPECT_EQ(client_socket.init(), qb::io::SocketStatus::Done);
        
        // Create endpoint for server
        qb::io::endpoint server_endpoint = qb::io::endpoint().as_in("127.0.0.1", udp_port);
        
        // We'll use the socket directly for UDP
        const std::string message = "Client to server via UDP";
        client_socket.write(message.c_str(), message.size(), server_endpoint);
        
        // Read response from server
        std::array<char, 100> buffer{};
        const std::string expected_response = "Server to client via UDP";
        
        // UDP is connectionless, so we need to wait a moment for the response
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        int bytes_read = client_socket.read(buffer.data(), buffer.size());
        EXPECT_GT(bytes_read, 0);
        
        std::string received_response(buffer.data(), bytes_read);
        EXPECT_EQ(received_response, expected_response);
        
        // Clean up
        client_socket.close();
    } catch (const std::exception& e) {
        std::cerr << "UDP Client error: " << e.what() << std::endl;
        FAIL() << "UDP Client operation failed";
    }
    
    server_thread.join();
}

// ----------------------- Stream Edge Cases -----------------------

// Test stream with large data transfer
TEST_F(StreamTest, LargeDataTransfer) {
    // Create a large data buffer (1MB)
    const size_t buffer_size = 1024 * 1024;
    std::vector<char> large_buffer(buffer_size, 'A');
    
    // Create output file
    std::string large_file = test_dir + "/large_transfer.dat";
    
    // Create stream with file
    qb::io::sys::file file;
    ASSERT_TRUE(file.open(large_file, O_RDWR | O_CREAT, 0644) >= 0);
    ASSERT_TRUE(file.is_open());
    
    qb::io::stream<qb::io::sys::file> stream;
    stream.transport() = file;
    
    // Write large buffer in chunks
    size_t total_written = 0;
    const size_t chunk_size = 8192; // Use smaller chunks
    
    while (total_written < buffer_size) {
        size_t current_chunk = std::min(chunk_size, buffer_size - total_written);
        stream.publish(large_buffer.data() + total_written, current_chunk);
        int bytes_written = stream.write();
        EXPECT_GT(bytes_written, 0);
        total_written += current_chunk;
    }
    
    // Verify file size
    file.close();
    EXPECT_EQ(std::filesystem::file_size(large_file), buffer_size);
    
    // Now read it back
    ASSERT_TRUE(file.open(large_file, O_RDONLY) >= 0);
    ASSERT_TRUE(file.is_open());
    
    stream.close();
    stream.transport() = file;
    
    std::vector<char> read_buffer(buffer_size);
    size_t total_read = 0;
    
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
    // Test reading from closed file
    qb::io::sys::file closed_file;
    qb::io::istream<qb::io::sys::file> input_stream;
    input_stream.transport() = closed_file;
    
    int result = input_stream.read();
    EXPECT_LT(result, 0);
    
    // Test writing to closed file
    qb::io::ostream<qb::io::sys::file> output_stream;
    output_stream.transport() = closed_file;
    
    const std::string test_data = "Test data";
    output_stream.publish(test_data.c_str(), test_data.size());
    result = output_stream.write();
    EXPECT_LT(result, 0);
    
    // Test operations on invalid socket
    qb::io::tcp::socket invalid_socket;
    qb::io::stream<qb::io::tcp::socket> socket_stream;
    socket_stream.transport() = invalid_socket;
    
    result = socket_stream.read();
    EXPECT_LT(result, 0);
    
    socket_stream.publish(test_data.c_str(), test_data.size());
    result = socket_stream.write();
    EXPECT_LT(result, 0);
}

// Run all the tests
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 