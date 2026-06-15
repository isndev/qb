/**
 * @file qb/io/tests/system/test-io.cpp
 * @brief Unit tests for I/O networking functionality
 *
 * This file contains tests for the I/O networking functionality in the QB framework,
 * including URI parsing, TCP/UDP socket communication in both blocking and non-blocking
 * modes, and Unix domain socket communication. It verifies proper socket creation,
 * connection, data transmission, and cleanup.
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
#include <qb/io/tcp/listener.h>
#include <qb/io/udp/socket.h>
#include <qb/io/protocol/json.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/stream.h>
#include <qb/io/transport/udp.h>
#include <qb/uuid.h>
#include <limits>
#include <set>
#include <thread>
#include <vector>
#include <unordered_set>

constexpr const unsigned short port = 64322;

TEST(URI, Resolving) {
    qb::io::uri u1{
        "https://www.example.com/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u1.scheme() == "https");
    EXPECT_TRUE(u1.host() == "www.example.com");
    EXPECT_TRUE(u1.path() == "/section1/section2/action");
    EXPECT_TRUE(u1.u_port() == 443);
    EXPECT_TRUE(u1.query("query1") == "value1");
    EXPECT_TRUE(u1.query("query2") == "value2");
    qb::io::uri u2{"https://www.example.com:8080/section1/section2/"
                   "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u2.scheme() == "https");
    EXPECT_TRUE(u2.host() == "www.example.com");
    EXPECT_TRUE(u2.path() == "/section1/section2/action");
    EXPECT_TRUE(u2.u_port() == 8080);
    EXPECT_TRUE(u2.query("query1") == "value1");
    EXPECT_TRUE(u2.query("query2") == "value2");
    qb::io::uri u3{
        "https://localhost/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u3.scheme() == "https");
    EXPECT_TRUE(u3.host() == "localhost");
    EXPECT_TRUE(u3.path() == "/section1/section2/action");
    EXPECT_TRUE(u3.u_port() == 443);
    EXPECT_TRUE(u3.query("query1") == "value1");
    EXPECT_TRUE(u3.query("query2") == "value2");
    qb::io::uri u4{
        "https://localhost:8080/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u4.scheme() == "https");
    EXPECT_TRUE(u4.host() == "localhost");
    EXPECT_TRUE(u4.path() == "/section1/section2/action");
    EXPECT_TRUE(u4.u_port() == 8080);
    EXPECT_TRUE(u4.query("query1") == "value1");
    EXPECT_TRUE(u4.query("query2") == "value2");
    qb::io::uri u5{
        "https://127.0.0.1/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u5.scheme() == "https");
    EXPECT_TRUE(u5.host() == "127.0.0.1");
    EXPECT_TRUE(u5.path() == "/section1/section2/action");
    EXPECT_TRUE(u5.u_port() == 443);
    EXPECT_TRUE(u5.query("query1") == "value1");
    EXPECT_TRUE(u5.query("query2") == "value2");
    qb::io::uri u6{
        "https://127.0.0.1:8080/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u6.scheme() == "https");
    EXPECT_TRUE(u6.host() == "127.0.0.1");
    EXPECT_TRUE(u6.path() == "/section1/section2/action");
    EXPECT_TRUE(u6.u_port() == 8080);
    EXPECT_TRUE(u6.query("query1") == "value1");
    EXPECT_TRUE(u6.query("query2") == "value2");
    qb::io::uri u7{"https://[::1]/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u7.scheme() == "https");
    EXPECT_TRUE(u7.host() == "::1");
    EXPECT_TRUE(u7.path() == "/section1/section2/action");
    EXPECT_TRUE(u7.u_port() == 443);
    EXPECT_TRUE(u7.query("query1") == "value1");
    EXPECT_TRUE(u7.query("query2") == "value2");
    EXPECT_TRUE(u7.af() == AF_INET6);
    qb::io::uri u8{
        "https://[::1]:8080/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u8.scheme() == "https");
    EXPECT_TRUE(u8.host() == "::1");
    EXPECT_TRUE(u8.path() == "/section1/section2/action");
    EXPECT_TRUE(u8.u_port() == 8080);
    EXPECT_TRUE(u8.query("query1") == "value1");
    EXPECT_TRUE(u8.query("query2") == "value2");
    EXPECT_TRUE(u8.af() == AF_INET6);
    qb::io::uri u9{"unix://name.sock/path/to/service/"};
    EXPECT_TRUE(u9.scheme() == "unix");
    EXPECT_TRUE(u9.host() == "name.sock");
    EXPECT_TRUE(u9.path() == "/path/to/service/");
    EXPECT_TRUE(u9.u_port() == 0);
    EXPECT_TRUE(u9.af() == AF_UNIX);
    qb::io::uri u10{"https://user:password@www.example.com/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u10.scheme() == "https");
    EXPECT_TRUE(u10.user_info() == "user:password");
    EXPECT_TRUE(u10.host() == "www.example.com");
    EXPECT_TRUE(u10.path() == "/section1/section2/action");
    EXPECT_TRUE(u10.u_port() == 443);
    EXPECT_TRUE(u10.query("query1") == "value1");
    EXPECT_TRUE(u10.query("query2") == "value2");
    qb::io::uri u20{"https://user:password@www.example.com:8080/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u20.scheme() == "https");
    EXPECT_TRUE(u20.user_info() == "user:password");
    EXPECT_TRUE(u20.host() == "www.example.com");
    EXPECT_TRUE(u20.path() == "/section1/section2/action");
    EXPECT_TRUE(u20.u_port() == 8080);
    EXPECT_TRUE(u20.query("query1") == "value1");
    EXPECT_TRUE(u20.query("query2") == "value2");
    qb::io::uri u30{"https://user:password@localhost/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u30.scheme() == "https");
    EXPECT_TRUE(u30.user_info() == "user:password");
    EXPECT_TRUE(u30.host() == "localhost");
    EXPECT_TRUE(u30.path() == "/section1/section2/action");
    EXPECT_TRUE(u30.u_port() == 443);
    EXPECT_TRUE(u30.query("query1") == "value1");
    EXPECT_TRUE(u30.query("query2") == "value2");
    qb::io::uri u40{"https://user:password@localhost:8080/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u40.scheme() == "https");
    EXPECT_TRUE(u40.user_info() == "user:password");
    EXPECT_TRUE(u40.host() == "localhost");
    EXPECT_TRUE(u40.path() == "/section1/section2/action");
    EXPECT_TRUE(u40.u_port() == 8080);
    EXPECT_TRUE(u40.query("query1") == "value1");
    EXPECT_TRUE(u40.query("query2") == "value2");
    qb::io::uri u50{"https://user:password@127.0.0.1/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u50.scheme() == "https");
    EXPECT_TRUE(u50.user_info() == "user:password");
    EXPECT_TRUE(u50.host() == "127.0.0.1");
    EXPECT_TRUE(u50.path() == "/section1/section2/action");
    EXPECT_TRUE(u50.u_port() == 443);
    EXPECT_TRUE(u50.query("query1") == "value1");
    EXPECT_TRUE(u50.query("query2") == "value2");
    qb::io::uri u60{"https://user:password@127.0.0.1:8080/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u60.scheme() == "https");
    EXPECT_TRUE(u60.user_info() == "user:password");
    EXPECT_TRUE(u60.host() == "127.0.0.1");
    EXPECT_TRUE(u60.path() == "/section1/section2/action");
    EXPECT_TRUE(u60.u_port() == 8080);
    EXPECT_TRUE(u60.query("query1") == "value1");
    EXPECT_TRUE(u60.query("query2") == "value2");
    qb::io::uri u70{"https://user:password@[::1]/section1/section2/"
                    "action?query1=value1&query2=value2"};
    EXPECT_TRUE(u70.scheme() == "https");
    EXPECT_TRUE(u70.user_info() == "user:password");
    EXPECT_TRUE(u70.host() == "::1");
    EXPECT_TRUE(u70.path() == "/section1/section2/action");
    EXPECT_TRUE(u70.u_port() == 443);
    EXPECT_TRUE(u70.query("query1") == "value1");
    EXPECT_TRUE(u70.query("query2") == "value2");
    EXPECT_TRUE(u70.af() == AF_INET6);
    qb::io::uri u80{"https://user:password@[::1]:8080/section1/section2/"
                    "action?query1%5B%5D=value1&query2%5B%5D=value2#fragment"};
    EXPECT_TRUE(u80.scheme() == "https");
    EXPECT_TRUE(u80.user_info() == "user:password");
    EXPECT_TRUE(u80.host() == "::1");
    EXPECT_TRUE(u80.path() == "/section1/section2/action");
    EXPECT_TRUE(u80.u_port() == 8080);
    EXPECT_TRUE(u80.query("query1[]") == "value1");
    EXPECT_TRUE(u80.query("query2[]") == "value2");
    EXPECT_TRUE(u80.fragment() == "fragment");
    EXPECT_TRUE(u80.af() == AF_INET6);
}

TEST(INET_TCP, Blocking) {
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_v4(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());
        EXPECT_EQ(listener.local_endpoint().port(), port);

        std::thread tsender([]() {
            qb::io::tcp::socket sock;
            EXPECT_FALSE(sock.connect_v4("127.0.0.1", port) !=
                         qb::io::SocketStatus::Done);
            EXPECT_TRUE(sock.is_open());
            EXPECT_EQ(sock.peer_endpoint().port(), port);

            const char msg[] = "Hello Test !";
            EXPECT_FALSE(sock.write(msg, sizeof(msg)) <= 0);
            sock.disconnect();
        });

        qb::io::tcp::socket sock;
        EXPECT_FALSE(listener.accept(sock) != qb::io::SocketStatus::Done);
        sock.set_nonblocking(false);

        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.read(buffer, 512) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
        tsender.join();
    });

    tlistener.join();
}

TEST(INET_TCP, NonBlocking) {
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_v6(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());
        EXPECT_EQ(listener.local_endpoint().port(), port);

        std::thread tsender([]() {
            qb::io::tcp::socket sock;
            EXPECT_FALSE(sock.connect_v6("::1", port) != qb::io::SocketStatus::Done);
            EXPECT_TRUE(sock.is_open());
            EXPECT_EQ(sock.peer_endpoint().port(), port);

            sock.set_nonblocking(true);

            const char msg[] = "Hello Test !";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            EXPECT_FALSE(sock.write(msg, sizeof(msg)) <= 0);
            sock.disconnect();
        });

        qb::io::tcp::socket sock;
        EXPECT_FALSE(listener.accept(sock) != qb::io::SocketStatus::Done);
        sock.set_nonblocking(true);

        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.read(buffer, 512) > 0);
        EXPECT_EQ(std::string(buffer), "");
        tsender.join();
    });

    tlistener.join();
}

TEST(INET_UDP, Blocking) {
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind_v4(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());
        EXPECT_EQ(listener.local_endpoint().port(), port);

        std::thread tsender([]() {
            qb::io::udp::socket sock;

            sock.init();
            EXPECT_TRUE(sock.is_open());

            const char msg[] = "Hello Test !";
            EXPECT_FALSE(sock.write(msg, sizeof(msg),
                                    qb::io::endpoint().as_in("127.0.0.1", port)) <= 0);
            sock.close();
        });

        char buffer[512];
        *buffer = 0;

        qb::io::endpoint peer;

        EXPECT_FALSE(listener.read(buffer, 512, peer) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
        EXPECT_EQ(peer.ip(), "127.0.0.1");
        tsender.join();
    });
    tlistener.join();
}

TEST(INET_UDP, NonBlocking) {
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind_v6(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());
        EXPECT_EQ(listener.local_endpoint().port(), port);
        listener.set_nonblocking(true);

        std::thread tsender([]() {
            qb::io::udp::socket sock;

            sock.init(AF_INET6);
            EXPECT_TRUE(sock.is_open());
            sock.set_nonblocking(true);

            const char msg[] = "Hello Test !";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            EXPECT_FALSE(sock.write(msg, sizeof(msg),
                                    qb::io::endpoint().as_in("::1", port)) <= 0);
            sock.close();
        });

        char buffer[512];
        *buffer = 0;

        qb::io::endpoint peer;

        EXPECT_FALSE(listener.read(buffer, 512, peer) > 0);
        EXPECT_EQ(std::string(buffer), "");
        EXPECT_EQ(peer.ip(), "");
        tsender.join();
    });
    tlistener.join();
}

#ifndef _WIN32

constexpr const char UNIX_SOCK_PATH[] = "./qb-io-test.sock";

TEST(UNIX_TCP, Blocking) {
    unlink(UNIX_SOCK_PATH);
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        std::thread tsender([]() {
            qb::io::tcp::socket sock;
            EXPECT_FALSE(sock.connect_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
            EXPECT_TRUE(sock.is_open());

            const char msg[] = "Hello Test !";
            char       buffer[512];
            *buffer = 0;

            EXPECT_FALSE(sock.read(buffer, sock.write(msg, sizeof(msg))) <= 0);
            EXPECT_TRUE(!strcmp(msg, buffer));
            sock.disconnect();
        });

        qb::io::tcp::socket sock = listener.accept();
        EXPECT_FALSE(sock.native_handle() <= 0);

        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.write(buffer, sock.read(buffer, 512)) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
        tsender.join();
    });

    tlistener.join();
}

TEST(UNIX_TCP, NonBlocking) {
    unlink(UNIX_SOCK_PATH);
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        std::thread tsender([]() {
            qb::io::tcp::socket sock;
            EXPECT_FALSE(sock.connect_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
            EXPECT_TRUE(sock.is_open());

            sock.set_nonblocking(true);

            const char msg[] = "Hello Test !";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            EXPECT_FALSE(sock.write(msg, sizeof(msg)) <= 0);
            sock.disconnect();
        });

        qb::io::tcp::socket sock;
        EXPECT_FALSE(listener.accept(sock) != qb::io::SocketStatus::Done);
        sock.set_nonblocking(true);

        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.read(buffer, 512) > 0);
        EXPECT_EQ(std::string(buffer), "");

        tsender.join();
    });

    tlistener.join();
}

TEST(UNIX_UDP, Blocking) {
    unlink(UNIX_SOCK_PATH);
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        std::thread tsender([]() {
            qb::io::udp::socket sock;

            sock.init(AF_UNIX);
            EXPECT_TRUE(sock.is_open());

            const char msg[] = "Hello Test !";
            EXPECT_FALSE(sock.write(msg, sizeof(msg),
                                    qb::io::endpoint().as_un(UNIX_SOCK_PATH)) <= 0);
            sock.close();
        });

        char buffer[512];
        *buffer = 0;

        qb::io::endpoint peer;

        EXPECT_FALSE(listener.read(buffer, 512, peer) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
        EXPECT_EQ(peer.ip(), "");
        tsender.join();
    });
    tlistener.join();
}

TEST(UNIX_UDP, NonBlocking) {
    unlink(UNIX_SOCK_PATH);
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind_un(UNIX_SOCK_PATH) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        listener.set_nonblocking(true);

        std::thread tsender([]() {
            qb::io::udp::socket sock;

            sock.init(AF_UNIX);
            EXPECT_TRUE(sock.is_open());
            sock.set_nonblocking(true);

            const char msg[] = "Hello Test !";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            EXPECT_FALSE(sock.write(msg, sizeof(msg),
                                    qb::io::endpoint().as_un(UNIX_SOCK_PATH)) <= 0);
            sock.close();
        });

        char buffer[512];
        *buffer = 0;

        qb::io::endpoint peer;

        EXPECT_FALSE(listener.read(buffer, 512, peer) > 0);
        EXPECT_EQ(std::string(buffer), "");
        EXPECT_EQ(peer.ip(), "");
        tsender.join();
    });
    tlistener.join();
}

#endif

//
// ---------------------------------------------------------------------------
// UUID thread safety
// ---------------------------------------------------------------------------

TEST(SocketUtils, UUIDThreadSafety) {
    constexpr int num_threads = 8;
    constexpr int uuids_per_thread = 500;

    std::vector<std::vector<qb::uuid>> results(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&results, t]() {
            results[t].reserve(uuids_per_thread);
            for (int i = 0; i < uuids_per_thread; ++i) {
                results[t].push_back(qb::generate_random_uuid());
            }
        });
    }

    for (auto &th : threads)
        th.join();

    std::set<std::string> all_uuids;
    for (auto &vec : results)
        for (auto &u : vec)
            all_uuids.insert(uuids::to_string(u));

    EXPECT_EQ(all_uuids.size(), static_cast<std::size_t>(num_threads * uuids_per_thread));
}

// ---------------------------------------------------------------------------
// Endpoint on invalid fd
// ---------------------------------------------------------------------------

TEST(SocketUtils, EndpointOnInvalidFd) {
    auto local = qb::io::socket::local_endpoint(qb::io::inet::invalid_socket);
    EXPECT_FALSE(local);

    auto peer = qb::io::socket::peer_endpoint(qb::io::inet::invalid_socket);
    EXPECT_FALSE(peer);
}

TEST(SocketUtils, EndpointOnValidConnection) {
    qb::io::tcp::listener listener;
    EXPECT_EQ(listener.listen_v4(64329), qb::io::SocketStatus::Done);

    std::thread t([]() {
        qb::io::tcp::socket sock;
        EXPECT_EQ(sock.connect_v4("127.0.0.1", 64329), qb::io::SocketStatus::Done);

        auto local = sock.local_endpoint();
        EXPECT_TRUE(local);
        EXPECT_NE(local.port(), 0);

        auto peer = sock.peer_endpoint();
        EXPECT_TRUE(peer);
        EXPECT_EQ(peer.port(), 64329);

        sock.disconnect();
    });

    qb::io::tcp::socket accepted;
    EXPECT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);
    t.join();
}

// ---------------------------------------------------------------------------
// UDP identity equality and hashing
// ---------------------------------------------------------------------------

TEST(UDPTransport, IdentityEquality) {
    qb::io::transport::udp::identity id1{qb::io::endpoint("127.0.0.1", 5000)};
    qb::io::transport::udp::identity id2{qb::io::endpoint("127.0.0.1", 5000)};
    qb::io::transport::udp::identity id3{qb::io::endpoint("127.0.0.1", 5001)};
    qb::io::transport::udp::identity id4{qb::io::endpoint("192.168.1.1", 5000)};

    EXPECT_EQ(id1, id2);
    EXPECT_NE(id1, id3);
    EXPECT_NE(id1, id4);
}

TEST(UDPTransport, IdentityHashConsistency) {
    qb::io::transport::udp::identity id1{qb::io::endpoint("127.0.0.1", 5000)};
    qb::io::transport::udp::identity id2{qb::io::endpoint("127.0.0.1", 5000)};
    qb::io::transport::udp::identity id3{qb::io::endpoint("10.0.0.1", 5000)};

    qb::io::transport::udp::identity::hasher h;
    EXPECT_EQ(h(id1), h(id2));
    EXPECT_NE(h(id1), h(id3));
}

TEST(UDPTransport, IdentityInUnorderedSet) {
    using identity = qb::io::transport::udp::identity;
    std::unordered_set<identity, identity::hasher> id_set;

    id_set.insert(identity{qb::io::endpoint("127.0.0.1", 5000)});
    id_set.insert(identity{qb::io::endpoint("127.0.0.1", 5001)});
    id_set.insert(identity{qb::io::endpoint("127.0.0.1", 5000)});

    EXPECT_EQ(id_set.size(), 2u);
}

TEST(UDPTransport, SmallReadLimitAcceptsSmallDatagram) {
    qb::io::transport::udp sender;
    qb::io::transport::udp receiver;

    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());

    constexpr unsigned short recv_port = 64328;
    ASSERT_EQ(receiver.transport().bind_v4(recv_port, "127.0.0.1"), 0);
    receiver.set_max_read_buffer_size(16);

    qb::io::transport::udp::identity dest{qb::io::endpoint("127.0.0.1", recv_port)};
    sender.setDestination(dest);

    constexpr char payload[] = "hello";
    ASSERT_NE(sender.publish(payload, 5), nullptr);
    ASSERT_EQ(sender.write(), 5);

    int ret = 0;
    for (int i = 0; i < 50 && ret <= 0; ++i) {
        ret = receiver.read();
        if (ret <= 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(ret, 5);
    EXPECT_EQ(receiver.pendingRead(), 5u);
    EXPECT_EQ(std::string_view(receiver.in().begin(), receiver.pendingRead()), "hello");
}

// Coroutine-based async TCP connection tests (C++23)
//

TEST(CORO_TCP, ConnectAwaiter) {
    using namespace std::chrono_literals;

    std::this_thread::sleep_for(50ms);

    constexpr const unsigned short test_port = 64323;

    std::thread tlistener([]() {
        qb::io::async::init();

        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_v4(test_port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        std::thread tconnector([]() {
            qb::io::async::init();

            bool completed = false;

            auto test_task = [&completed]() -> qb::io::async::task<void> {
                // Test basic connection using coroutine awaiter
                auto socket = co_await qb::io::async::tcp::connect(
                    qb::io::uri{std::string{"tcp://127.0.0.1:"} + std::to_string(test_port)}
                );

                // Connection should succeed
                EXPECT_TRUE(socket.has_value());
                EXPECT_TRUE(socket->is_open());

                completed = true;
            };

            qb::io::async::coro_scheduler().spawn(test_task());

            // Run until completed
            while (!completed) {
                qb::io::async::run(EVRUN_NOWAIT);
            }
        });

        // Accept the connection
        qb::io::tcp::socket accepted_sock;
        EXPECT_FALSE(listener.accept(accepted_sock) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(accepted_sock.is_open());

        tconnector.join();
    });

    tlistener.join();
}

TEST(CORO_TCP, ConnectAwaiterTimeout) {
    using namespace std::chrono_literals;

    std::this_thread::sleep_for(50ms);

    // Test connection to non-existent port with short timeout
    qb::io::async::init();

    bool completed = false;

    auto test_task = [&completed]() -> qb::io::async::task<void> {
        // Try to connect to a port that should be closed, with 100ms timeout
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{"tcp://127.0.0.1:1"},  // Port 1 is unlikely to be open
            100ms
        );

        // Connection should fail (timeout or refused)
        EXPECT_FALSE(socket.has_value());

        completed = true;
    };

    qb::io::async::coro_scheduler().spawn(test_task());

    // Run until completed (with some extra time for timeout)
    auto start = std::chrono::steady_clock::now();
    while (!completed) {
        qb::io::async::run(EVRUN_NOWAIT);

        // Safety timeout for the test itself
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > 5s) {
            ADD_FAILURE() << "Test timed out waiting for connection";
            break;
        }
    }

    EXPECT_TRUE(completed);
}

TEST(CORO_TCP, DISABLED_ConnectAwaiterWithExistingSocket) {
    using namespace std::chrono_literals;

    // Small delay to let previous test free the port
    std::this_thread::sleep_for(100ms);

    constexpr const unsigned short test_port = 64327;

    std::thread tlistener([]() {
        qb::io::async::init();

        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen_v4(test_port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.is_open());

        std::thread tconnector([]() {
            qb::io::async::init();

            bool completed = false;
            std::optional<qb::io::tcp::socket> connected_socket;

            auto test_task = [&completed, &connected_socket]() -> qb::io::async::task<void> {
                // Create a socket first (initialized but not connected)
                qb::io::tcp::socket existing_socket;
                auto status = existing_socket.init(AF_INET);
                EXPECT_FALSE(status != qb::io::SocketStatus::Done);

                // Connect using the existing socket
                connected_socket = co_await qb::io::async::tcp::connect_with_socket(
                    std::move(existing_socket),
                    qb::io::uri{std::string{"tcp://127.0.0.1:"} + std::to_string(test_port)}
                );

                completed = true;
            };

            qb::io::async::coro_scheduler().spawn(test_task());

            // Run until completed with timeout
            auto start = std::chrono::steady_clock::now();
            while (!completed) {
                qb::io::async::run(EVRUN_NOWAIT);

                auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed > 5s) {
                    ADD_FAILURE() << "Test timed out";
                    break;
                }
            }

            EXPECT_TRUE(connected_socket.has_value());
            if (connected_socket) {
                EXPECT_TRUE(connected_socket->is_open());
            }
        });

        // Accept the connection
        qb::io::tcp::socket accepted_sock;
        EXPECT_FALSE(listener.accept(accepted_sock) != qb::io::SocketStatus::Done);

        tconnector.join();
    });

    tlistener.join();
}

// ===========================================================================
// Regression tests for bugs found during code review
// ===========================================================================

// ---------------------------------------------------------------------------
// BUG FIX: base_pipe copy after free_front copied from wrong offset
// ---------------------------------------------------------------------------

TEST(PipeRegression, CopyAfterFreeFront) {
    qb::allocator::pipe<char> src;
    src.put("GARBAGE_PREFIX_HELLO_WORLD", 26);

    src.free_front(15);
    ASSERT_EQ(src.size(), 11u);
    EXPECT_EQ(src.view(), "HELLO_WORLD");

    qb::allocator::pipe<char> dst(src);
    EXPECT_EQ(dst.size(), 11u);
    EXPECT_EQ(dst.view(), "HELLO_WORLD");
}

TEST(PipeRegression, AssignAfterFreeFront) {
    qb::allocator::pipe<char> src;
    src.put("PREFIX_DATA_PAYLOAD", 19);
    src.free_front(12);
    ASSERT_EQ(src.view(), "PAYLOAD");

    qb::allocator::pipe<char> dst;
    dst.put("overwritten", 11);
    dst = src;

    EXPECT_EQ(dst.size(), 7u);
    EXPECT_EQ(dst.view(), "PAYLOAD");
}

TEST(PipeRegression, CopyAfterMultipleFreeFronts) {
    qb::allocator::pipe<char> p;
    for (int i = 0; i < 5; ++i) {
        p.put("ABCDEFGHIJ", 10);
    }
    p.free_front(40);
    ASSERT_EQ(p.size(), 10u);
    EXPECT_EQ(p.view(), "ABCDEFGHIJ");

    auto copy = p;
    EXPECT_EQ(copy.size(), 10u);
    EXPECT_EQ(copy.view(), "ABCDEFGHIJ");
}

// ---------------------------------------------------------------------------
// BUG FIX: uri::decode buffer over-read on truncated % sequence
// ---------------------------------------------------------------------------

TEST(URIRegression, DecodeTrailingPercent) {
    auto result1 = qb::io::uri::decode(std::string_view("hello%"));
    EXPECT_EQ(result1, "hello%");

    auto result2 = qb::io::uri::decode(std::string_view("hello%2"));
    EXPECT_EQ(result2, "hello%2");

    auto result3 = qb::io::uri::decode(std::string_view("%20end%"));
    EXPECT_EQ(result3, " end%");

    auto result4 = qb::io::uri::decode(std::string_view(""));
    EXPECT_EQ(result4, "");

    auto result5 = qb::io::uri::decode(std::string_view("%"));
    EXPECT_EQ(result5, "%");
}

// ---------------------------------------------------------------------------
// BUG FIX: uri::parse() didn't reset string_view members on re-parse
// ---------------------------------------------------------------------------

TEST(URIRegression, ReassignClearsStaleComponents) {
    qb::io::uri u{"https://user:pass@host.com:9090/path?q=v#frag"};
    EXPECT_EQ(u.scheme(), "https");
    EXPECT_EQ(u.user_info(), "user:pass");
    EXPECT_EQ(u.host(), "host.com");
    EXPECT_EQ(u.u_port(), 9090);
    EXPECT_EQ(u.path(), "/path");
    EXPECT_EQ(u.query("q"), "v");
    EXPECT_EQ(u.fragment(), "frag");

    u = std::string("http://minimal.com");
    EXPECT_EQ(u.scheme(), "http");
    EXPECT_EQ(u.host(), "minimal.com");
    EXPECT_EQ(u.u_port(), 80);
    EXPECT_EQ(u.path(), "/");

    EXPECT_TRUE(u.user_info().empty());
    EXPECT_TRUE(u.fragment().empty());
    EXPECT_TRUE(u.queries().empty());
}

TEST(URIRegression, ReassignToEmpty) {
    qb::io::uri u{"https://example.com:443/api?key=val#sec"};
    EXPECT_FALSE(u.scheme().empty());
    EXPECT_FALSE(u.host().empty());

    u = std::string("");
    EXPECT_TRUE(u.scheme().empty());
    EXPECT_TRUE(u.host().empty());
    EXPECT_TRUE(u.fragment().empty());
    EXPECT_EQ(u.path(), "/");
}

// ---------------------------------------------------------------------------
// BUG FIX: uri copy/move assignment didn't preserve explicit _af
// ---------------------------------------------------------------------------

TEST(URIRegression, CopyPreservesAF) {
    qb::io::uri ipv6_uri{"tcp://[::1]:5000/path"};
    EXPECT_EQ(ipv6_uri.af(), AF_INET6);

    qb::io::uri copy;
    copy = ipv6_uri;
    EXPECT_EQ(copy.af(), AF_INET6);
    EXPECT_EQ(copy.host(), "::1");
}

TEST(URIRegression, MovePreservesAF) {
    qb::io::uri unix_uri{"unix:///var/run/app.sock"};
    EXPECT_EQ(unix_uri.af(), AF_UNIX);

    qb::io::uri moved;
    moved = std::move(unix_uri);
    EXPECT_EQ(moved.af(), AF_UNIX);
    EXPECT_EQ(moved.scheme(), "unix");
}

TEST(URIRegression, CopyConstructPreservesAF) {
    qb::io::uri ipv6{"https://[::1]:443/api"};
    EXPECT_EQ(ipv6.af(), AF_INET6);

    qb::io::uri copy(ipv6);
    EXPECT_EQ(copy.af(), AF_INET6);
    EXPECT_EQ(copy.host(), "::1");
    EXPECT_EQ(copy.u_port(), 443);
}

TEST(URIRegression, MoveConstructPreservesAF) {
    qb::io::uri unix_uri{"unix://my.sock/service"};
    EXPECT_EQ(unix_uri.af(), AF_UNIX);

    qb::io::uri moved(std::move(unix_uri));
    EXPECT_EQ(moved.af(), AF_UNIX);
    EXPECT_EQ(moved.scheme(), "unix");
}

// ===========================================================================
// Pipe robustness tests
// ===========================================================================

TEST(PipeRobustness, EmptyPipeOperations) {
    qb::allocator::pipe<char> p;
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.size(), 0u);
    EXPECT_EQ(p.begin(), p.end());
    EXPECT_EQ(p.view(), "");
    EXPECT_EQ(p.str(), "");
    EXPECT_GT(p.capacity(), 0u);
}

TEST(PipeRobustness, SwapCorrectness) {
    qb::allocator::pipe<int> a;
    int vals_a[] = {1, 2, 3};
    a.put(vals_a, 3);

    qb::allocator::pipe<int> b;
    int vals_b[] = {10, 20, 30, 40};
    b.put(vals_b, 4);

    a.swap(b);
    EXPECT_EQ(a.size(), 4u);
    EXPECT_EQ(a.begin()[0], 10);
    EXPECT_EQ(a.begin()[3], 40);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b.begin()[0], 1);
}

TEST(PipeRobustness, SwapBothNonEmpty) {
    qb::allocator::pipe<int> a;
    int vals_a[] = {1, 2, 3, 4, 5};
    a.put(vals_a, 5);

    qb::allocator::pipe<int> b;
    int vals_b[] = {100, 200};
    b.put(vals_b, 2);

    a.swap(b);
    EXPECT_EQ(a.size(), 2u);
    EXPECT_EQ(a.begin()[0], 100);
    EXPECT_EQ(a.begin()[1], 200);
    EXPECT_EQ(b.size(), 5u);
    EXPECT_EQ(b.begin()[0], 1);
    EXPECT_EQ(b.begin()[4], 5);
}

TEST(PipeRobustness, ResizeGrow) {
    qb::allocator::pipe<char> p;
    p.put("ABC", 3);
    EXPECT_EQ(p.size(), 3u);

    p.resize(10);
    EXPECT_EQ(p.size(), 10u);
    EXPECT_EQ(std::string_view(p.begin(), 3), "ABC");
}

TEST(PipeRobustness, ResizeShrink) {
    qb::allocator::pipe<char> p;
    p.put("ABCDEFGHIJ", 10);
    EXPECT_EQ(p.size(), 10u);

    p.resize(5);
    EXPECT_EQ(p.size(), 5u);
    EXPECT_EQ(std::string_view(p.begin(), 5), "ABCDE");
}

TEST(PipeRobustness, ReorderAfterFreeFront) {
    qb::allocator::pipe<char> p;
    p.put("HEADERPAYLOAD", 13);
    p.free_front(6);
    EXPECT_EQ(p.view(), "PAYLOAD");

    p.reorder();
    EXPECT_EQ(p.view(), "PAYLOAD");
    EXPECT_EQ(p.size(), 7u);
}

TEST(PipeRobustness, PutStringView) {
    qb::allocator::pipe<char> p;
    std::string_view sv = "hello from string_view";
    p.put(sv);
    EXPECT_EQ(p.view(), sv);
}

TEST(PipeRobustness, PutCString) {
    qb::allocator::pipe<char> p;
    const char* cstr = "c-string data";
    p.put(cstr);
    EXPECT_EQ(p.view(), "c-string data");
}

TEST(PipeRobustness, PutStdString) {
    qb::allocator::pipe<char> p;
    std::string s = "std::string content";
    p.put(s);
    EXPECT_EQ(p.view(), s);
}

TEST(PipeRobustness, PutPipe) {
    qb::allocator::pipe<char> src;
    src.put("source_pipe_data", 16);

    qb::allocator::pipe<char> dst;
    dst.put(src);
    EXPECT_EQ(dst.view(), "source_pipe_data");
}

TEST(PipeRobustness, PutPipeAfterFreeFront) {
    qb::allocator::pipe<char> src;
    src.put("GARBAGE_REAL_DATA", 17);
    src.free_front(8);

    qb::allocator::pipe<char> dst;
    dst.put(src);
    EXPECT_EQ(dst.view(), "REAL_DATA");
}

TEST(PipeRobustness, ReserveDoesNotChangeSize) {
    qb::allocator::pipe<char> p;
    p.put("data", 4);
    auto old_size = p.size();
    p.reserve(1000);
    EXPECT_EQ(p.size(), old_size);
    EXPECT_EQ(p.view(), "data");
}

TEST(PipeRobustness, AllocateBackOverflowThrows) {
    qb::allocator::pipe<char> p;
    EXPECT_THROW(p.allocate_back(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
}

TEST(PipeRobustness, MoveConstruct) {
    qb::allocator::pipe<char> src;
    src.put("MOVE_ME", 7);

    qb::allocator::pipe<char> dst(std::move(src));
    EXPECT_EQ(dst.view(), "MOVE_ME");
    EXPECT_EQ(dst.size(), 7u);
    EXPECT_TRUE(src.empty());
}

TEST(PipeRobustness, MoveAssign) {
    qb::allocator::pipe<char> src;
    src.put("MOVE_ASSIGN", 11);

    qb::allocator::pipe<char> dst;
    dst.put("OLD_DATA", 8);
    dst = std::move(src);

    EXPECT_EQ(dst.view(), "MOVE_ASSIGN");
    EXPECT_TRUE(src.empty());
}

// ===========================================================================
// Stream buffer limit tests
// ===========================================================================

TEST(StreamLimits, DefaultBufferLimitsAreConfigured) {
    qb::io::stream<qb::io::tcp::socket> s;
    EXPECT_EQ(s.max_read_buffer_size(), QB_MAX_READ_BUFFER_SIZE);
    EXPECT_EQ(s.max_write_buffer_size(), QB_MAX_WRITE_BUFFER_SIZE);
    EXPECT_GT(s.max_read_buffer_size(), 0u);
    EXPECT_LT(s.max_read_buffer_size(), std::numeric_limits<std::size_t>::max());
}

TEST(StreamLimits, PublishRejectsWhenLimitExceeded) {
    qb::io::stream<qb::io::tcp::socket> s;
    s.set_max_write_buffer_size(20);

    const char data[] = "1234567890";
    auto *result1 = s.publish(data, 10);
    EXPECT_NE(result1, nullptr);
    EXPECT_EQ(s.pendingWrite(), 10u);

    auto *result2 = s.publish(data, 10);
    EXPECT_NE(result2, nullptr);
    EXPECT_EQ(s.pendingWrite(), 20u);

    // This should be rejected — would exceed the 20-byte limit
    auto *result3 = s.publish(data, 1);
    EXPECT_EQ(result3, nullptr);
    EXPECT_EQ(s.pendingWrite(), 20u);
}

TEST(StreamLimits, PublishAcceptsExactLimit) {
    qb::io::stream<qb::io::tcp::socket> s;
    s.set_max_write_buffer_size(10);

    const char data[] = "1234567890";
    auto *result = s.publish(data, 10);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(s.pendingWrite(), 10u);
}

TEST(StreamLimits, SetMaxBufferSizes) {
    qb::io::stream<qb::io::tcp::socket> s;

    s.set_max_read_buffer_size(1024);
    EXPECT_EQ(s.max_read_buffer_size(), 1024u);

    s.set_max_write_buffer_size(2048);
    EXPECT_EQ(s.max_write_buffer_size(), 2048u);
}

// ===========================================================================
// URI decode iterator template tests
// ===========================================================================

TEST(URIRobustness, DecodeIteratorTrailingPercent) {
    // Template decode breaks on truncated % — stops processing, no crash
    std::string input1 = "abc%";
    auto result1 = qb::io::uri::decode(input1.begin(), input1.end());
    EXPECT_EQ(result1, "abc");

    std::string input2 = "%";
    auto result2 = qb::io::uri::decode(input2.begin(), input2.end());
    EXPECT_EQ(result2, "");

    std::string input3 = "test%2";
    auto result3 = qb::io::uri::decode(input3.begin(), input3.end());
    EXPECT_EQ(result3, "test");

    // Verify no crash on empty range
    std::string empty;
    auto result4 = qb::io::uri::decode(empty.begin(), empty.end());
    EXPECT_EQ(result4, "");

    // Valid decode still works
    std::string valid = "hello%20world";
    auto result5 = qb::io::uri::decode(valid.begin(), valid.end());
    EXPECT_EQ(result5, "hello world");
}

TEST(URIRobustness, DecodeStringViewPreservesPercent) {
    // string_view overload preserves truncated % in output
    EXPECT_EQ(qb::io::uri::decode(std::string_view("abc%")), "abc%");
    EXPECT_EQ(qb::io::uri::decode(std::string_view("%")), "%");
    EXPECT_EQ(qb::io::uri::decode(std::string_view("test%2")), "test%2");
    EXPECT_EQ(qb::io::uri::decode(std::string_view("%20ok%")), " ok%");
}

TEST(URIRobustness, DecodeIteratorValid) {
    std::string input = "%48%65%6C%6C%6F";
    auto result = qb::io::uri::decode(input.begin(), input.end());
    EXPECT_EQ(result, "Hello");
}

TEST(URIRobustness, EncodeDecodeRoundtrip) {
    std::vector<std::string> test_cases = {
        "simple text",
        "special: !@#$%^&*()",
        "",
        "unicode: \xC3\xA9\xC3\xA0\xC3\xBC",
        "slashes/and?query=yes&more=true",
        std::string(1000, 'X'),
        "trailing%",
        "%already%20encoded",
    };

    for (const auto &original : test_cases) {
        auto encoded = qb::io::uri::encode(original);
        auto decoded = qb::io::uri::decode(encoded);
        EXPECT_EQ(decoded, original) << "Roundtrip failed for: " << original;
    }
}

TEST(URIRobustness, ParseQueryWithEncodedAmpersand) {
    qb::io::uri u{"http://host/p?key=val%26ue&k2=v2"};
    EXPECT_EQ(u.query("key"), "val&ue");
    EXPECT_EQ(u.query("k2"), "v2");
}

TEST(URIRobustness, ParseEmptyQueryValues) {
    qb::io::uri u{"http://host/p?a=&b=&c="};
    EXPECT_EQ(u.query("a"), "");
    EXPECT_EQ(u.query("b"), "");
    EXPECT_EQ(u.query("c"), "");
}

TEST(URIRobustness, ParseQueryKeyOnly) {
    qb::io::uri u{"http://host/p?flagA&flagB&key=val"};
    EXPECT_EQ(u.query("flagA"), "");
    EXPECT_EQ(u.query("flagB"), "");
    EXPECT_EQ(u.query("key"), "val");
}

TEST(URIRobustness, LongURIStress) {
    std::string long_query;
    for (int i = 0; i < 200; ++i) {
        if (i > 0) long_query += "&";
        long_query += "key" + std::to_string(i) + "=value" + std::to_string(i);
    }
    qb::io::uri u{"http://host/path?" + long_query};
    EXPECT_EQ(u.query("key0"), "value0");
    EXPECT_EQ(u.query("key99"), "value99");
    EXPECT_EQ(u.query("key199"), "value199");
}

// is_valid() must report structural parse failures the constructor swallows.
TEST(URIRobustness, IsValidReportsParseFailures) {
    // Well-formed URIs are valid.
    EXPECT_TRUE(qb::io::uri("https://host:8080/path?a=b").is_valid());
    EXPECT_TRUE(qb::io::uri("unix://name.sock/svc").is_valid());
    EXPECT_TRUE(qb::io::uri("").is_valid()); // empty → path "/", still valid

    // Unclosed IPv6 bracket → invalid.
    EXPECT_FALSE(qb::io::uri("http://[::1/path").is_valid());
    // Control character in the path → invalid.
    EXPECT_FALSE(qb::io::uri(std::string("http://host/pa\x01th")).is_valid());

    // Validity is recomputed on assignment (no stale state).
    qb::io::uri u("http://[::1/bad");
    EXPECT_FALSE(u.is_valid());
    u = std::string("http://good/path");
    EXPECT_TRUE(u.is_valid());
}

TEST(URIRobustness, PortRejectsOutOfRangeTruncation) {
    // Valid explicit ports parse exactly.
    EXPECT_EQ(qb::io::uri("http://host:8080/").u_port(), 8080);
    EXPECT_EQ(qb::io::uri("http://host:65535/").u_port(), 65535);

    // Out-of-range explicit ports must NOT silently truncate (wrap-around):
    // "99999" previously became static_cast<uint16_t>(99999) == 34463. It and
    // any other all-digit value above 65535 must now be rejected as 0.
    EXPECT_EQ(qb::io::uri("http://host:65536/").u_port(), 0);
    EXPECT_EQ(qb::io::uri("http://host:99999/").u_port(), 0);
    // Beyond INT_MAX too (from_chars reports result_out_of_range, not overflow).
    EXPECT_EQ(qb::io::uri("http://host:4294967296/").u_port(), 0);
    EXPECT_EQ(qb::io::uri("http://host:99999999999999999999/").u_port(), 0);
}

// JSON protocol DoS guard: pathologically nested input is rejected before the
// recursive parser can blow the stack. String-aware so brackets inside strings
// do not count toward depth.
TEST(JsonProtocol, DepthGuard) {
    using qb::protocol::detail::json_depth_within;
    constexpr std::size_t kMax = qb::protocol::detail::kJsonMaxNestingDepth;

    // Reasonable nesting passes.
    std::string ok = R"({"a":{"b":[1,2,{"c":3}]}})";
    EXPECT_TRUE(json_depth_within(ok.data(), ok.size(), kMax));

    // Brackets inside strings must NOT count toward depth.
    std::string in_string = R"({"k":"[[[[[[[[[[ not real nesting ]]]]]]]]]]"})";
    EXPECT_TRUE(json_depth_within(in_string.data(), in_string.size(), 4));

    // Escaped quote inside a string keeps the scanner in-string.
    std::string esc = R"({"k":"a\"[[[[[[ b"})";
    EXPECT_TRUE(json_depth_within(esc.data(), esc.size(), 2));

    // Pathological nesting beyond the limit is rejected.
    std::string bomb(kMax + 5, '[');
    EXPECT_FALSE(json_depth_within(bomb.data(), bomb.size(), kMax));

    // Exactly at the limit is accepted; one over is not.
    std::string at_limit(kMax, '[');
    EXPECT_TRUE(json_depth_within(at_limit.data(), at_limit.size(), kMax));
    std::string over(kMax + 1, '[');
    EXPECT_FALSE(json_depth_within(over.data(), over.size(), kMax));
}

TEST(JsonProtocol, MsgpackDepthGuard) {
    using qb::protocol::detail::msgpack_depth_within;
    constexpr std::size_t kMax = qb::protocol::detail::kJsonMaxNestingDepth;
    // 0x91 = fixarray(1 element); 0xc0 = nil. N nested single-element arrays
    // give nesting depth N. The msgpack json_packed protocol must bound this
    // before from_msgpack()'s recursive reader blows the stack.

    // Reasonable nesting passes: [[[42]]].
    std::string ok = {char(0x91), char(0x91), char(0x91), char(0x2a)};
    EXPECT_TRUE(msgpack_depth_within(ok.data(), ok.size(), kMax));

    // Pathological nesting beyond the limit is rejected.
    std::string bomb(kMax + 5, char(0x91));
    bomb.push_back(char(0xc0));
    EXPECT_FALSE(msgpack_depth_within(bomb.data(), bomb.size(), kMax));

    // Exactly at the limit is accepted; one over is not.
    std::string at_limit(kMax, char(0x91));
    at_limit.push_back(char(0xc0));
    EXPECT_TRUE(msgpack_depth_within(at_limit.data(), at_limit.size(), kMax));
    std::string over_mp(kMax + 1, char(0x91));
    over_mp.push_back(char(0xc0));
    EXPECT_FALSE(msgpack_depth_within(over_mp.data(), over_mp.size(), kMax));
}
