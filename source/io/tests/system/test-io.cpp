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
#include <thread>

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
    EXPECT_TRUE(u7.host() == "[::1]");
    EXPECT_TRUE(u7.path() == "/section1/section2/action");
    EXPECT_TRUE(u7.u_port() == 443);
    EXPECT_TRUE(u7.query("query1") == "value1");
    EXPECT_TRUE(u7.query("query2") == "value2");
    EXPECT_TRUE(u7.af() == AF_INET6);
    qb::io::uri u8{
        "https://[::1]:8080/section1/section2/action?query1=value1&query2=value2"};
    EXPECT_TRUE(u8.scheme() == "https");
    EXPECT_TRUE(u8.host() == "[::1]");
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
    EXPECT_TRUE(u70.host() == "[::1]");
    EXPECT_TRUE(u70.path() == "/section1/section2/action");
    EXPECT_TRUE(u70.u_port() == 443);
    EXPECT_TRUE(u70.query("query1") == "value1");
    EXPECT_TRUE(u70.query("query2") == "value2");
    EXPECT_TRUE(u70.af() == AF_INET6);
    qb::io::uri u80{"https://user:password@[::1]:8080/section1/section2/"
                    "action?query1%5B%5D=value1&query2%5B%5D=value2#fragment"};
    EXPECT_TRUE(u80.scheme() == "https");
    EXPECT_TRUE(u80.user_info() == "user:password");
    EXPECT_TRUE(u80.host() == "[::1]");
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