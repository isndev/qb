/**
 * @file qb/io/tests/system/test-async-io.cpp
 * @brief Unit tests for asynchronous I/O operations
 * 
 * This file contains tests for the core asynchronous I/O functionality in the QB framework,
 * including timers, event handling, signal processing, and file watching capabilities.
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
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <thread>
#include <fstream>
#include <csignal>
#include <qb/io/tcp/socket.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/system/file.h>
#include <fcntl.h>
#include <iostream>

using namespace qb::io;

// Test fixture for async I/O tests
class AsyncIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        async::init();
    }

    void TearDown() override {
        // Cleanup after each test
    }
};

// Test basic timer functionality
class TimerHandler : public async::with_timeout<TimerHandler> {
public:
    std::atomic<bool> timer_triggered{false};
    std::atomic<int> timer_count{0};
    
    explicit TimerHandler(double timeout = 0.1)
        : with_timeout(timeout) {}
    
    void on(async::event::timer const &) {
        timer_triggered = true;
        timer_count++;
    }
};

TEST_F(AsyncIOTest, BasicTimer) {
    TimerHandler timer(0.1); // 100ms timeout
    
    // Run event loop for a short time
    for (int i = 0; i < 5 && !timer.timer_triggered; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    EXPECT_TRUE(timer.timer_triggered);
    EXPECT_GE(timer.timer_count, 1);
}

TEST_F(AsyncIOTest, UpdateTimeout) {
    TimerHandler timer(1.0); // 1s timeout - increase the delay
    
    // Run the event loop once to initialize
    async::run(EVRUN_NOWAIT);
    
    // Update the timeout after a short delay
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    timer.updateTimeout();
    
    // Make sure the timer is not triggered too early
    for (int i = 0; i < 3; ++i) {
        async::run(EVRUN_NOWAIT); // Use NOWAIT to avoid blocking
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Verify that the timer has not been triggered
    EXPECT_FALSE(timer.timer_triggered);
    
    // Wait until the timeout is exceeded
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    
    // Run the event loop to allow the timer to trigger
    for (int i = 0; i < 5 && !timer.timer_triggered; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    EXPECT_TRUE(timer.timer_triggered);
}

TEST_F(AsyncIOTest, SetTimeout) {
    TimerHandler timer(1.0); // Initial 1s timeout
    
    // Change timeout to 0.1s
    timer.setTimeout(0.1);
    
    // Run event loop
    for (int i = 0; i < 5 && !timer.timer_triggered; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    EXPECT_TRUE(timer.timer_triggered);
    EXPECT_DOUBLE_EQ(timer.getTimeout(), 0.1);
    
    // Disable timeout
    timer.timer_triggered = false;
    timer.setTimeout(0.0);
    
    // Run event loop - timer should not trigger
    for (int i = 0; i < 5; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    EXPECT_FALSE(timer.timer_triggered);
    EXPECT_DOUBLE_EQ(timer.getTimeout(), 0.0);
}

// Test the async::Timeout utility class
TEST_F(AsyncIOTest, TimeoutUtility) {
    std::atomic<bool> callback_executed{false};
    
    // Create a timeout that will execute after 100ms
    new async::Timeout<std::function<void()>>(
        [&callback_executed]() {
            callback_executed = true;
        },
        0.1
    );
    
    // Run event loop until callback is executed
    for (int i = 0; i < 10 && !callback_executed; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    EXPECT_TRUE(callback_executed);
}

// Test immediate execution with Timeout utility
TEST_F(AsyncIOTest, ImmediateTimeoutUtility) {
    std::atomic<bool> callback_executed{false};
    
    // Create a timeout that will execute immediately (timeout = 0)
    new async::Timeout<std::function<void()>>(
        [&callback_executed]() {
            callback_executed = true;
        },
        0.0
    );
    
    // Should be executed immediately without running the event loop
    EXPECT_TRUE(callback_executed);
}

// Test signal handling
class SignalHandler {
public:
    std::atomic<bool> sigint_received{false};
    std::atomic<bool> sigusr1_received{false};
    
    SignalHandler() {
        // Register signal handlers
        sigint_watcher = new async::event::signal<SIGINT>(async::listener::current.loop());
        sigusr1_watcher = new async::event::signal<SIGUSR1>(async::listener::current.loop());
        
        // Set callbacks
        sigint_watcher->set<SignalHandler, &SignalHandler::handle_sigint>(this);
        sigusr1_watcher->set<SignalHandler, &SignalHandler::handle_sigusr1>(this);
        
        // Start watchers
        sigint_watcher->start();
        sigusr1_watcher->start();
    }
    
    ~SignalHandler() {
        if (sigint_watcher) {
            sigint_watcher->stop();
            delete sigint_watcher;
        }
        if (sigusr1_watcher) {
            sigusr1_watcher->stop();
            delete sigusr1_watcher;
        }
    }
    
    void handle_sigint(ev::sig &, int) {
        sigint_received = true;
    }
    
    void handle_sigusr1(ev::sig &, int) {
        sigusr1_received = true;
    }
    
private:
    async::event::signal<SIGINT> *sigint_watcher = nullptr;
    async::event::signal<SIGUSR1> *sigusr1_watcher = nullptr;
};

#ifndef _WIN32
TEST_F(AsyncIOTest, SignalHandling) {
    SignalHandler handler;
    
    // Create a thread to send signals
    std::thread signal_thread([&]() {
        // Wait a bit before sending signals
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Send SIGUSR1 signal
        kill(getpid(), SIGUSR1);
        
        // Wait a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Send SIGINT signal
        kill(getpid(), SIGINT);
    });
    
    // Run event loop until signals are received
    for (int i = 0; i < 20 && (!handler.sigint_received || !handler.sigusr1_received); ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    signal_thread.join();
    
    EXPECT_TRUE(handler.sigusr1_received);
    EXPECT_TRUE(handler.sigint_received);
}
#endif

// Simple client class for TCP tests
class SimpleClient {
public:
    std::atomic<bool> connected{false};
    std::atomic<bool> data_received{false};
    std::string received_data;
    tcp::socket socket;
    
    SimpleClient() {
        // Initialize the socket in blocking mode by default
        socket.init();
    }
    
    bool connect(const std::string& ip, unsigned short port) {
        // Use a blocking connection for more reliable tests
        int result = socket.connect_v4(ip, port);
        if (result == 0) {
            // Then set the socket as non-blocking for the rest of the test
            socket.set_nonblocking(true);
            connected = true;
            return true;
        }
        return false;
    }
    
    bool send(const std::string& data) {
        return socket.write(data.c_str(), data.size()) == static_cast<int>(data.size());
    }
    
    void receive() {
        char buffer[1024] = {0};
        auto received = socket.read(buffer, sizeof(buffer) - 1);
        if (received > 0) {
            buffer[received] = 0;
            received_data = buffer;
            data_received = true;
        }
    }
};

// Simple server class for TCP tests
class SimpleServer {
public:
    std::atomic<bool> client_connected{false};
    std::atomic<bool> data_received{false};
    std::string received_data;
    tcp::listener listener;
    tcp::socket client_socket;
    
    SimpleServer() {
    }
    
    bool listen(unsigned short port) {
        return listener.listen_v4(port) == 0;
    }
    
    bool accept() {
        auto status = listener.accept(client_socket);
        if (status == 0) {
            client_socket.set_nonblocking(true);
            client_connected = true;
            return true;
        }
        return false;
    }
    
    bool send(const std::string& data) {
        return client_socket.write(data.c_str(), data.size()) == static_cast<int>(data.size());
    }
    
    void receive() {
        char buffer[1024] = {0};
        auto received = client_socket.read(buffer, sizeof(buffer) - 1);
        if (received > 0) {
            buffer[received] = 0;
            received_data = buffer;
            data_received = true;
        }
    }
};

TEST_F(AsyncIOTest, TCPNonBlockingIO) {
    const unsigned short TEST_PORT = 9876;
    const std::string TEST_MESSAGE = "Hello, QB Async IO!";
    const std::string RESPONSE_MESSAGE = "Hello from server!";
    
    // Set up the server
    SimpleServer server;
    
    // Listen on the test port
    if (!server.listen(TEST_PORT)) {
        GTEST_SKIP() << "Failed to set up TCP server, skipping test";
    }
    
    // Give the server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Create a client and try to connect
    SimpleClient client;
    if (!client.connect("127.0.0.1", TEST_PORT)) {
        GTEST_SKIP() << "Failed to connect to TCP server, skipping test";
    }
    
    // Accept the connection on the server side
    bool server_accepted = false;
    for (int i = 0; i < 20; ++i) {
        if (server.accept()) {
            server_accepted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!server_accepted) {
        GTEST_SKIP() << "Server failed to accept connection, skipping test";
    }
    
    // Client sends a message to the server
    ASSERT_TRUE(client.send(TEST_MESSAGE));
    
    // Server receives the message
    for (int i = 0; i < 20 && !server.data_received; ++i) {
        server.receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(server.data_received);
    EXPECT_EQ(server.received_data, TEST_MESSAGE);
    
    // Server sends a response
    ASSERT_TRUE(server.send(RESPONSE_MESSAGE));
    
    // Client receives the response
    for (int i = 0; i < 20 && !client.data_received; ++i) {
        client.receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(client.data_received);
    EXPECT_EQ(client.received_data, RESPONSE_MESSAGE);
}

// Test for file operations
TEST_F(AsyncIOTest, FileOperations) {
    // Create a test file
    const std::string test_file = "test_file_operations.txt";
    const std::string content = "Test content for file operations";
    
    {
        std::ofstream file(test_file);
        file << content;
        file.close();
    }
    
    // Test file IO
    sys::file file;
    ASSERT_NE(file.open(test_file, O_RDONLY), -1);
    ASSERT_TRUE(file.is_open());
    
    char buffer[100] = {0};
    ASSERT_EQ(file.read(buffer, sizeof(buffer) - 1), content.size());
    EXPECT_EQ(std::string(buffer), content);
    
    file.close();
    
    // Test file write
    ASSERT_NE(file.open(test_file, O_WRONLY | O_TRUNC), -1);
    ASSERT_TRUE(file.is_open());
    const std::string new_content = "New test content";
    ASSERT_EQ(file.write(new_content.c_str(), new_content.size()), new_content.size());
    file.close();
    
    // Verify written content
    ASSERT_NE(file.open(test_file, O_RDONLY), -1);
    ASSERT_TRUE(file.is_open());
    memset(buffer, 0, sizeof(buffer));
    ASSERT_EQ(file.read(buffer, sizeof(buffer) - 1), new_content.size());
    EXPECT_EQ(std::string(buffer), new_content);
    file.close();
    
    // Cleanup
    std::remove(test_file.c_str());
}

// Test event priorities
TEST_F(AsyncIOTest, EventPriorities) {
    std::vector<int> execution_order;
    std::mutex mutex;
    
    // Create events with different timeouts that will execute in order
    new async::Timeout<std::function<void()>>(
        [&execution_order, &mutex]() {
            std::lock_guard<std::mutex> lock(mutex);
            execution_order.push_back(1);
        },
        0.1
    );
    
    new async::Timeout<std::function<void()>>(
        [&execution_order, &mutex]() {
            std::lock_guard<std::mutex> lock(mutex);
            execution_order.push_back(2);
        },
        0.2
    );
    
    new async::Timeout<std::function<void()>>(
        [&execution_order, &mutex]() {
            std::lock_guard<std::mutex> lock(mutex);
            execution_order.push_back(3);
        },
        0.3
    );
    
    // Run event loop
    for (int i = 0; i < 30; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        std::lock_guard<std::mutex> lock(mutex);
        if (execution_order.size() >= 3) {
            break;
        }
    }
    
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(execution_order.size(), 3);
    // They should execute in order of timeouts
    EXPECT_EQ(execution_order[0], 1);
    EXPECT_EQ(execution_order[1], 2);
    EXPECT_EQ(execution_order[2], 3);
}

// Constants for text-based communication test
constexpr const unsigned short TEXT_PROTOCOL_PORT = 9877;
constexpr const char TEXT_MESSAGE[] = "Hello, Text Protocol!";
std::atomic<std::size_t> msg_count_server = 0;
std::atomic<std::size_t> msg_count_client = 0;
constexpr const std::size_t TEXT_ITERATIONS = 10;

// Test with a text protocol (similar to test-session-text.cpp)
class TextServer;

class TextServerClient : public use<TextServerClient>::tcp::client<TextServer> {
public:
    using Protocol = qb::protocol::text::command<TextServerClient>;

    explicit TextServerClient(IOServer &server)
        : client(server) {}

    ~TextServerClient() {
        EXPECT_EQ(msg_count_server, TEXT_ITERATIONS);
    }

    void on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        *this << msg.text << Protocol::end;
        ++msg_count_server;
    }
};

class TextServer : public use<TextServer>::tcp::server<TextServerClient> {
    std::size_t connection_count = 0u;

public:
    ~TextServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void on(IOSession &) {
        ++connection_count;
    }
};

class TextClient : public use<TextClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<TextClient>;

    ~TextClient() {
        EXPECT_EQ(msg_count_client, TEXT_ITERATIONS);
    }

    void on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        ++msg_count_client;
    }
};

TEST_F(AsyncIOTest, TextProtocolCommunication) {
    async::init();
    msg_count_server = 0;
    msg_count_client = 0;

    TextServer server;
    server.transport().listen_v4(TEXT_PROTOCOL_PORT);
    server.start();

    std::thread client_thread([]() {
        async::init();
        TextClient client;
        
        if (SocketStatus::Done != client.transport().connect_v4("127.0.0.1", TEXT_PROTOCOL_PORT)) {
            throw std::runtime_error("could not connect to text server");
        }
        
        client.start();

        // Send multiple messages
        for (auto i = 0u; i < TEXT_ITERATIONS; ++i) {
            client << TEXT_MESSAGE << '\n';
        }

        // Run event loop until all messages are processed
        for (auto i = 0; i < (TEXT_ITERATIONS * 5) && 
             (msg_count_server < TEXT_ITERATIONS || msg_count_client < TEXT_ITERATIONS); ++i) {
            async::run(EVRUN_ONCE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    // Run server event loop
    for (auto i = 0; i < (TEXT_ITERATIONS * 5) && 
         (msg_count_server < TEXT_ITERATIONS || msg_count_client < TEXT_ITERATIONS); ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    client_thread.join();
    
    EXPECT_EQ(msg_count_server, TEXT_ITERATIONS);
    EXPECT_EQ(msg_count_client, TEXT_ITERATIONS);
}

#ifdef QB_IO_WITH_SSL
// Test SSL/TLS communication
class SecureServer;

class SecureServerClient : public use<SecureServerClient>::tcp::ssl::client<SecureServer> {
public:
    using Protocol = qb::protocol::text::command<SecureServerClient>;

    explicit SecureServerClient(IOServer &server)
        : client(server) {}

    ~SecureServerClient() {
        EXPECT_EQ(msg_count_server, TEXT_ITERATIONS);
    }

    void on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        *this << msg.text << Protocol::end;
        ++msg_count_server;
    }
};

class SecureServer : public use<SecureServer>::tcp::ssl::server<SecureServerClient> {
    std::size_t connection_count = 0u;

public:
    ~SecureServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void on(IOSession &) {
        ++connection_count;
    }
};

class SecureClient : public use<SecureClient>::tcp::ssl::client<> {
public:
    using Protocol = qb::protocol::text::command<SecureClient>;

    ~SecureClient() {
        EXPECT_EQ(msg_count_client, TEXT_ITERATIONS);
    }

    void on(Protocol::message &&msg) {
        EXPECT_EQ(msg.text.size(), sizeof(TEXT_MESSAGE) - 1);
        ++msg_count_client;
    }
};

TEST_F(AsyncIOTest, SSLCommunication) {
    std::cout << "Starting SSLCommunication test" << std::endl;
    
    // Check for certificate files
    const std::string cert_file = "./cert.pem";
    const std::string key_file = "./key.pem";
    
    std::ifstream cert_check(cert_file);
    std::ifstream key_check(key_file);
    if (!cert_check.good() || !key_check.good()) {
        GTEST_SKIP() << "SSL certificate or key file not found, skipping test";
        return;
    }
    
    // Reset counters
    async::init();
    msg_count_server = 0;
    msg_count_client = 0;

    // Set up server
    SecureServer server;
    server.transport().init(
        ssl::create_server_context(SSLv23_server_method(), cert_file, key_file));
    server.transport().listen_v4(9878);
    server.start();

    // Client thread
    std::thread client_thread([]() {
        async::init();
        SecureClient client;
        
        if (SocketStatus::Done != client.transport().connect_v4("127.0.0.1", 9878)) {
            throw std::runtime_error("could not connect to secure server");
        }
        
        client.start();

        // Send multiple messages
        for (auto i = 0u; i < TEXT_ITERATIONS; ++i) {
            client << TEXT_MESSAGE << '\n';
        }

        // Run event loop until all messages are processed
        for (auto i = 0; i < (TEXT_ITERATIONS * 5) && 
             (msg_count_server < TEXT_ITERATIONS || msg_count_client < TEXT_ITERATIONS); ++i) {
            async::run(EVRUN_ONCE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    // Run server event loop
    for (auto i = 0; i < (TEXT_ITERATIONS * 5) && 
         (msg_count_server < TEXT_ITERATIONS || msg_count_client < TEXT_ITERATIONS); ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    client_thread.join();
    
    EXPECT_EQ(msg_count_server, TEXT_ITERATIONS);
    EXPECT_EQ(msg_count_client, TEXT_ITERATIONS);
}
#endif

// Test file monitoring functionality
class FileWatchHandler {
public:
    std::atomic<bool> file_changed{false};
    std::string filename;
    ev::stat *stat_watcher = nullptr;
    
    FileWatchHandler(const std::string &path) : filename(path) {
        stat_watcher = new ev::stat(async::listener::current.loop());
        stat_watcher->set<FileWatchHandler, &FileWatchHandler::handle_file_change>(this);
        
        // Start watching the file for modifications
        stat_watcher->set(filename.c_str());
        stat_watcher->start();
    }
    
    ~FileWatchHandler() {
        if (stat_watcher) {
            stat_watcher->stop();
            delete stat_watcher;
        }
    }
    
    void handle_file_change(ev::stat &, int events) {
        file_changed = true;
    }
};

TEST_F(AsyncIOTest, FileWatcherFunctionality) {
    // Create a test file
    const std::string test_file = "test_file_watcher.txt";
    const std::string initial_content = "Initial test content";
    const std::string modified_content = "Modified test content";
    
    {
        std::ofstream file(test_file);
        file << initial_content;
        file.close();
    }
    
    // Set up a file watcher
    FileWatchHandler watcher(test_file);
    
    // Run event loop a few times to initialize the watcher
    for (int i = 0; i < 5; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Ensure watcher isn't triggered yet
    EXPECT_FALSE(watcher.file_changed);
    
    // Modify the file
    {
        // Make sure to wait a moment to ensure the timestamp changes
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        std::ofstream file(test_file);
        file << modified_content;
        file.close();
    }
    
    // Run event loop to detect changes
    for (int i = 0; i < 20 && !watcher.file_changed; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Verify change was detected
    EXPECT_TRUE(watcher.file_changed);
    
    // Cleanup
    std::remove(test_file.c_str());
}

// Test for the async::io::sys::file class (not async::file as this class is different)
TEST_F(AsyncIOTest, AsyncFileOperations) {
    // Create a test file
    const std::string test_file = "test_async_file_io.txt";
    const std::string file_content = "Async file operations test content";
    
    {
        std::ofstream file(test_file);
        file << file_content;
        file.close();
    }
    
    // Test asynchronous file operations
    sys::file file;
    ASSERT_TRUE(file.open(test_file, O_RDONLY) >= 0);
    
    // Set to non-blocking mode
    file.set_non_blocking(true);
    
    // Read content
    char buffer[1024] = {0};
    auto bytes_read = file.read(buffer, sizeof(buffer) - 1);
    EXPECT_GT(bytes_read, 0);
    EXPECT_EQ(std::string(buffer), file_content);
    
    // Close the file
    file.close();
    
    // Delete the file
    std::remove(test_file.c_str());
}

// Test UDP communication with datagram-based API
TEST_F(AsyncIOTest, UDPDatagram) {
    const unsigned short UDP_PORT = 9879;
    const std::string UDP_MESSAGE = "Hello, UDP Async IO!";
    
    // Create and bind a UDP socket for sending
    udp::socket send_socket;
    ASSERT_TRUE(send_socket.init());
    
    // Create and bind a UDP socket for receiving
    udp::socket recv_socket;
    ASSERT_TRUE(recv_socket.init());
    ASSERT_EQ(recv_socket.bind_v4(UDP_PORT), 0);
    
    // Set up the destination endpoint
    endpoint dest;
    dest.as_in("127.0.0.1", UDP_PORT);
    
    // Send a message
    ASSERT_EQ(send_socket.write(UDP_MESSAGE.c_str(), UDP_MESSAGE.size(), dest), 
              static_cast<int>(UDP_MESSAGE.size()));
    
    // Receive the message
    char buffer[1024] = {0};
    endpoint sender;
    int received = recv_socket.read(buffer, sizeof(buffer) - 1, sender);
    
    ASSERT_GT(received, 0);
    EXPECT_EQ(std::string(buffer, received), UDP_MESSAGE);
}

// Test periodic timer functionality
class PeriodicTimerHandler {
public:
    std::atomic<int> timer_count{0};
    ev::timer *timer_watcher = nullptr;
    
    PeriodicTimerHandler(double interval) {
        timer_watcher = new ev::timer(async::listener::current.loop());
        timer_watcher->set<PeriodicTimerHandler, &PeriodicTimerHandler::handle_timer>(this);
        
        // Start timer with 0 initial delay and specified interval for repeat
        timer_watcher->start(0.0, interval);
    }
    
    ~PeriodicTimerHandler() {
        if (timer_watcher) {
            timer_watcher->stop();
            delete timer_watcher;
        }
    }
    
    void handle_timer(ev::timer &, int) {
        timer_count++;
    }
    
    void stop() {
        if (timer_watcher) {
            timer_watcher->stop();
        }
    }
};

TEST_F(AsyncIOTest, PeriodicTimer) {
    // Create a periodic timer that triggers every 50ms
    PeriodicTimerHandler timer(0.05);
    
    // Run event loop to allow timer to trigger multiple times
    for (int i = 0; i < 10; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    // Timer should have been triggered multiple times
    EXPECT_GE(timer.timer_count, 3);
    
    // Now stop the timer
    timer.stop();
    
    // Remember the count
    int previous_count = timer.timer_count;
    
    // Run event loop again
    for (int i = 0; i < 5; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    // Timer count should not have increased
    EXPECT_EQ(timer.timer_count, previous_count);
}

// Test timer cancellation
class CancellableTimerHandler : public async::with_timeout<CancellableTimerHandler> {
public:
    std::atomic<bool> timer_triggered{false};
    
    explicit CancellableTimerHandler(double timeout = 0.5)
        : with_timeout(timeout) {}
    
    void on(async::event::timer const &) {
        timer_triggered = true;
    }
    
    // Method to forcefully stop the timer
    void stop() {
        // From the with_timeout class, we can see the timer is controlled by _async_event
        // The setTimeout(0.0) should stop it, but we'll force it to be sure
        this->_async_event.stop();
    }
};

TEST_F(AsyncIOTest, TimerCancellation) {
    CancellableTimerHandler timer(0.2); // 200ms timeout
    
    // Run the event loop a few times but not enough to trigger the timer
    for (int i = 0; i < 3; ++i) {
        async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Cancel the timer by setting timeout to 0
    timer.setTimeout(0.0);
    
    // Force the timer stop (this is needed since setTimeout may not immediately stop the timer)
    timer.stop();
    
    // Reset the flag in case it was triggered
    timer.timer_triggered = false;
    
    // Now run the event loop long enough that the original timer would have triggered
    for (int i = 0; i < 10; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    
    // Timer should not have been triggered
    EXPECT_FALSE(timer.timer_triggered);
}

// Test multiple concurrent timers
TEST_F(AsyncIOTest, MultipleConcurrentTimers) {
    std::atomic<bool> timer1_triggered{false};
    std::atomic<bool> timer2_triggered{false};
    std::atomic<bool> timer3_triggered{false};
    
    // Create timers with different timeouts
    new async::Timeout<std::function<void()>>(
        [&timer1_triggered]() { timer1_triggered = true; },
        0.05  // 50ms
    );
    
    new async::Timeout<std::function<void()>>(
        [&timer2_triggered]() { timer2_triggered = true; },
        0.1   // 100ms
    );
    
    new async::Timeout<std::function<void()>>(
        [&timer3_triggered]() { timer3_triggered = true; },
        0.15  // 150ms
    );
    
    // Run event loop until all timers trigger
    for (int i = 0; i < 20 && (!timer1_triggered || !timer2_triggered || !timer3_triggered); ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_TRUE(timer1_triggered);
    EXPECT_TRUE(timer2_triggered);
    EXPECT_TRUE(timer3_triggered);
}

// Test timer precision
TEST_F(AsyncIOTest, TimerPrecision) {
    using clock = std::chrono::high_resolution_clock;
    
    std::atomic<bool> timer_triggered{false};
    const double timeout_seconds = 0.1; // 100ms timeout
    
    auto start_time = clock::now();
    
    new async::Timeout<std::function<void()>>(
        [&timer_triggered]() { timer_triggered = true; },
        timeout_seconds
    );
    
    // Run event loop until timer triggers
    for (int i = 0; i < 20 && !timer_triggered; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto end_time = clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    EXPECT_TRUE(timer_triggered);
    
    // Allow some flexibility in timing, but should be reasonably close to the target
    EXPECT_GE(elapsed_ms, timeout_seconds * 1000 * 0.8);  // At least 80% of the timeout
    EXPECT_LE(elapsed_ms, timeout_seconds * 1000 * 1.5);  // At most 150% of the timeout
}

// Class to test synchronization using standard C++ mutexes between timers
class MutexTester {
public:
    std::vector<int> results;
    std::mutex result_mutex;
    
    void critical_section(int id) {
        // Use standard C++ mutex to protect the results vector
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            results.push_back(id);
        }
    }
};

// Test synchronization between async timers
TEST_F(AsyncIOTest, TimerSynchronization) {
    MutexTester tester;
    
    // Create multiple timers accessing the same resource
    for (int i = 0; i < 5; ++i) {
        new async::Timeout<std::function<void()>>(
            [&tester, i]() {
                tester.critical_section(i);
            },
            0.05 * (i + 1)  // Staggered timeouts
        );
    }
    
    // Run event loop until all timers complete
    for (int i = 0; i < 30; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Verify that all timers executed
    EXPECT_EQ(tester.results.size(), 5);
    
    // Check that all expected values are present
    std::vector<int> expected{0, 1, 2, 3, 4};
    std::sort(tester.results.begin(), tester.results.end());
    EXPECT_EQ(tester.results, expected);
}

// Test for multiple threads using async operations
TEST_F(AsyncIOTest, MultiThreadedAsyncOperations) {
    const int NUM_THREADS = 4;
    const int ITERATIONS_PER_THREAD = 5;
    
    std::vector<std::thread> threads;
    std::atomic<int> total_completed{0};
    
    // Create multiple threads, each with their own timers
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&total_completed, t, ITERATIONS_PER_THREAD]() {
            // Initialize async in this thread
            async::init();
            
            // Create a timer to trigger after a small delay
            std::atomic<int> completed{0};
            
            for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
                // Create a timeout that will execute after a small delay
                new async::Timeout<std::function<void()>>(
                    [&completed, i, t]() {
                        // This is executed when the timer triggers
                        completed++;
                    },
                    0.05 * (t + 1)  // Different timeout for each thread
                );
                
                // Run the event loop a few times
                for (int j = 0; j < 5 && completed <= i; ++j) {
                    async::run(EVRUN_ONCE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            
            total_completed += completed;
        });
    }
    
    // Wait for all threads to complete
    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Verify all timers completed
    EXPECT_EQ(total_completed, NUM_THREADS * ITERATIONS_PER_THREAD);
}

// Test checking if the event loop is alive
TEST_F(AsyncIOTest, EventLoopAlive) {
    // Initial state should be alive since async::init() is called in SetUp
    
    // Check that we can run the event loop without errors
    EXPECT_NO_THROW({
        async::run(EVRUN_NOWAIT);
        async::run(EVRUN_ONCE);
    });
    
    // Re-init should also work fine
    EXPECT_NO_THROW(async::init());
}

// Test nested timed operations
TEST_F(AsyncIOTest, NestedTimedOperations) {
    std::atomic<int> operation_count{0};
    
    // Create a timer that will spawn a nested timer when triggered
    new async::Timeout<std::function<void()>>(
        [&operation_count]() {
            operation_count++;
            
            // Create a nested timer
            new async::Timeout<std::function<void()>>(
                [&operation_count]() {
                    operation_count++;
                },
                0.05 // 50ms
            );
        },
        0.05 // 50ms
    );
    
    // Run event loop until both timers have triggered
    for (int i = 0; i < 20 && operation_count < 2; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_EQ(operation_count, 2);
}

// Test for custom stateful timeout operations
class StateHolder {
public:
    int state_value{42};
};

class StatefulTimer : public async::with_timeout<StatefulTimer> {
public:
    StateHolder state;
    std::atomic<bool> timer_triggered{false};
    
    explicit StatefulTimer(double timeout = 0.1)
        : with_timeout(timeout) {}
    
    void on(async::event::timer const &) {
        // Verify state is intact
        EXPECT_EQ(state.state_value, 42);
        state.state_value = 84;
        timer_triggered = true;
    }
};

TEST_F(AsyncIOTest, StatefulTimerOperation) {
    StatefulTimer timer(0.1); // 100ms timeout
    
    // Run event loop until timer triggers
    for (int i = 0; i < 10 && !timer.timer_triggered; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    EXPECT_TRUE(timer.timer_triggered);
    EXPECT_EQ(timer.state.state_value, 84);
}

// Test for handling of dropped timers
TEST_F(AsyncIOTest, DroppedTimers) {
    std::atomic<int> completed_count{0};
    
    // Create multiple timeout objects but don't store references to them
    for (int i = 0; i < 10; ++i) {
        new async::Timeout<std::function<void()>>(
            [&completed_count]() {
                completed_count++;
            },
            0.02 * (i + 1)  // Stagger timeouts from 20ms to 200ms
        );
    }
    
    // Run event loop until all timers have triggered
    for (int i = 0; i < 30 && completed_count < 10; ++i) {
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_EQ(completed_count, 10);
}

// Test async initialization and cleanup in multiple threads
TEST_F(AsyncIOTest, AsyncInitCleanupThreads) {
    std::atomic<int> init_success{0};
    std::atomic<int> run_success{0};
    
    const int num_threads = 4;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&init_success, &run_success]() {
            // Initialize async in this thread
            async::init();
            init_success++;
            
            // Run event loop once
            async::run(EVRUN_NOWAIT);
            run_success++;
            
            // Clean up (implicit when thread terminates)
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(init_success, num_threads);
    EXPECT_EQ(run_success, num_threads);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 