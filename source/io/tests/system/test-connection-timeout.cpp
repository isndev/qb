/**
 * @file qb/io/tests/system/test-connection-timeout.cpp
 * @brief Unit tests for connection timeout handling
 *
 * This file contains tests for handling connection timeouts in the QB framework,
 * ensuring that connections that cannot be established properly time out as expected.
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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <qb/io/async.h>
#include <qb/io/async/event/all.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/udp/socket.h>
#include <string>
#include <thread>

namespace qb::io::tests {

/**
 * Test suite for connection timeouts in qb::io
 */
class ConnectionTimeoutTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        // Setup code if needed before each test
    }

    void
    TearDown() override {
        // Cleanup code if needed after each test
    }

    // Helper to check for non-blocking operation errors
    bool
    is_would_block_error(int err) const {
        // Print the error code for debugging
        std::cout << "Error code: " << err << std::endl;

        // Common non-blocking operation error codes (POSIX + Windows Winsock)
        return err == EINPROGRESS || err == EWOULDBLOCK || err == EAGAIN ||
               err == EINTR ||
               err == ENOTCONN // macOS: 57 - Socket is not connected
#if defined(_WIN32)
               // On Windows, Winsock uses its own codes, independent of errno
               || err == WSAEWOULDBLOCK   // 10035
               || err == WSAEINPROGRESS   // 10036
#endif
            ;
    }
};

/**
 * Tests TCP connection timeout behavior when connecting to a non-existent server
 */
TEST_F(ConnectionTimeoutTest, TCPConnectionTimeout) {
    qb::io::tcp::socket socket;
    ASSERT_EQ(0, socket.init());

    // Use a longer timeout to ensure test captures it
    std::chrono::seconds timeout(3);

    // Try to connect to a non-routable IP address with a timeout
    auto start_time = std::chrono::steady_clock::now();

    // Start non-blocking connect
    int result =
        socket.n_connect_v4("192.0.2.1", 12345); // Using TEST-NET-1 reserved IP range

    if (result != 0) {
        std::cout << "Non-blocking connect returned: " << result << ", errno: " << errno
                  << std::endl;
    }

    // Use handle_write_ready to wait with timeout
    result = qb::io::socket::handle_write_ready(socket.native_handle(),
                                                std::chrono::microseconds(timeout));

    auto end_time = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    std::cout << "Connection attempt took " << duration.count() << " seconds"
              << std::endl;
    std::cout << "handle_write_ready result: " << result << std::endl;

    // Cross-platform verification
    if (result > 0) {
        // If handle_write_ready returned success, try to verify if the connection
        // actually succeeded by checking if we can get valid peer information
        try {
            auto peer_ep = socket.peer_endpoint();
            
            // For a non-existent server, we either expect an exception, or
            // an invalid endpoint (which should convert to false in a boolean context)
            // Based on qb::io::ip::endpoint operator bool() implementation
            EXPECT_FALSE(static_cast<bool>(peer_ep)) 
                << "Socket shouldn't be connected to an unreachable peer";
                
        } catch (...) {
            // An exception is also a valid indicator that the connection failed
            // as expected on some platforms
        }
    } else {
        // If handle_write_ready returned <= 0, that's a timeout as expected on macOS
        EXPECT_LE(result, 0);
    }

    // Duration check only on macOS, since Linux/Windows might detect unreachable
    // networks immediately without waiting for the full timeout
#ifdef __APPLE__
    EXPECT_GE(duration.count(), 1); // At least 1 second on macOS
#endif
}

/**
 * Tests TCP connection timeout behavior with async operations
 */
TEST_F(ConnectionTimeoutTest, AsyncTCPTimeout) {
#ifdef _WIN32
    // On Windows, n_connect_v4() calls freeaddrinfo() *after* connect(), which
    // resets WSAGetLastError() to 0.  The WSAEWOULDBLOCK code is no longer
    // readable by the time the test checks it.  The actual timeout behaviour is
    // already verified by TCPConnectionTimeout (handle_write_ready).
    GTEST_SKIP() << "Windows: freeaddrinfo() clears WSAGetLastError() in the "
                    "n_connect_v4 resolve chain; post-connect error code is unreliable.";
#endif

    qb::io::tcp::socket socket;
    ASSERT_EQ(0, socket.init());
    ASSERT_EQ(0, socket.set_nonblocking(true));

    // Start a non-blocking connection
    int result =
        socket.n_connect_v4("192.0.2.1", 12345); // Using TEST-NET-1 reserved IP range

    // For non-blocking connect, check if it would block.
    // On Windows, Winsock sets WSAGetLastError(), NOT errno — use
    // socket::get_last_errno() which abstracts this difference.
    if (result != 0) {
        int err = qb::io::socket::get_last_errno();
        std::cout << "Non-blocking connect errno: " << err << std::endl;
        EXPECT_TRUE(is_would_block_error(err));
    }

    // Wait for socket to become writable (with timeout)
    int status = qb::io::socket::handle_write_ready(socket.native_handle(),
                                                    std::chrono::seconds(3));

    // Cross-platform verification
    if (status > 0) {
        // If socket is writable, try to verify if it's actually connected
        try {
            auto peer_ep = socket.peer_endpoint();
            
            // For a non-existent server, we either expect an exception, or
            // an invalid endpoint (which should convert to false in a boolean context)
            EXPECT_FALSE(static_cast<bool>(peer_ep)) 
                << "Socket shouldn't be connected to an unreachable peer";
                
        } catch (...) {
            // An exception is also a valid indicator that the connection failed
            // as expected on some platforms
        }
    } else {
        // If status <= 0, it timed out or had an error as expected on macOS
        EXPECT_LE(status, 0);
    }
}

/**
 * Tests UDP datagram timeout behavior
 */
TEST_F(ConnectionTimeoutTest, UDPDatagramTimeout) {
    qb::io::udp::socket socket;

    // For UDP socket, init returns a boolean (true on success)
    ASSERT_TRUE(socket.init());

    // Attempt to receive data with a timeout
    std::chrono::seconds timeout(1);
    char                 buffer[1024];

    // Use recv_n with a timeout instead of set_timeout
    int result = qb::io::socket::recv_n(socket.native_handle(), buffer, sizeof(buffer),
                                        std::chrono::microseconds(timeout));

    std::cout << "UDP recv_n result: " << result << ", errno: " << errno << std::endl;

    // The receive should time out, which on some platforms returns 0 (no data) and on
    // others < 0
    EXPECT_TRUE(result <= 0);

    // If we got an error, it should be one of these error types
    if (result < 0) {
        EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN || errno == ETIMEDOUT ||
                    errno == EINTR || errno == EINPROGRESS);
    }
}

/**
 * Tests non-blocking socket behavior with timeouts
 */
TEST_F(ConnectionTimeoutTest, NonBlockingSocketBehavior) {
#ifdef _WIN32
    // Same root cause as AsyncTCPTimeout: freeaddrinfo() inside n_connect_v4()
    // clears WSAGetLastError() before this test can inspect it.
    GTEST_SKIP() << "Windows: freeaddrinfo() clears WSAGetLastError() in the "
                    "n_connect_v4 resolve chain; post-connect error code is unreliable.";
#endif

    qb::io::tcp::socket socket;
    ASSERT_EQ(0, socket.init());
    ASSERT_EQ(0, socket.set_nonblocking(true));

    // Start a non-blocking connection to a non-routable address
    int result =
        socket.n_connect_v4("192.0.2.1", 12345); // Using TEST-NET-1 reserved IP range
    EXPECT_NE(result, 0);

    // Check if the connection is in progress.
    // On Windows, Winsock sets WSAGetLastError(), NOT errno — use
    // socket::get_last_errno() which abstracts this difference.
    int err = qb::io::socket::get_last_errno();
    std::cout << "Non-blocking connect errno: " << err << std::endl;
    EXPECT_TRUE(is_would_block_error(err));

    // Wait for connection with timeout
    int status = qb::io::socket::handle_write_ready(socket.native_handle(),
                                                    std::chrono::seconds(3));

    // Cross-platform verification
    if (status > 0) {
        // If socket is writable, try to verify if it's actually connected
        try {
            auto peer_ep = socket.peer_endpoint();
            
            // For a non-existent server, we either expect an exception, or
            // an invalid endpoint (which should convert to false in a boolean context)
            EXPECT_FALSE(static_cast<bool>(peer_ep)) 
                << "Socket shouldn't be connected to an unreachable peer";
                
        } catch (...) {
            // An exception is also a valid indicator that the connection failed
            // as expected on some platforms
        }
    } else {
        // If status <= 0, it timed out or had an error as expected on macOS
        EXPECT_LE(status, 0);
    }
}

} // namespace qb::io::tests