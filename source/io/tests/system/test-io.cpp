/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <gtest/gtest.h>
#include <thread>
#include <qb/io/ip.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/udp/socket.h>

constexpr const unsigned short port = 64322;

TEST(IP, Resolving) {
    EXPECT_EQ(qb::io::ip::Any, qb::io::ip(0, 0, 0, 0));
    EXPECT_GT(qb::io::ip::None, qb::io::ip::Any);
    EXPECT_LT(qb::io::ip::Any, qb::io::ip::None);
    EXPECT_GE(qb::io::ip::None, qb::io::ip::Any);
    EXPECT_LE(qb::io::ip::Any, qb::io::ip::None);;
    EXPECT_EQ(qb::io::ip::None, qb::io::ip("255.255.255.255"));
    EXPECT_EQ(qb::io::ip::LocalHost, qb::io::ip(std::string("127.0.0.1")));
    EXPECT_EQ(qb::io::ip("localhost"), qb::io::ip(std::string("127.0.0.1")));
    EXPECT_EQ(qb::io::ip("192.168.0.123").toString(), "192.168.0.123");
    EXPECT_NE(qb::io::ip::None, qb::io::ip("google.com"));
}

TEST(TCP, Blocking) {
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.good());
        EXPECT_EQ(listener.getLocalPort(), port);
        qb::io::tcp::socket sock;
        EXPECT_FALSE(listener.accept(sock) != qb::io::SocketStatus::Done);
        sock.setBlocking(true);
        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.read(buffer, 512) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::thread tsender([]() {
        qb::io::tcp::socket sock;
        EXPECT_FALSE(sock.connect("127.0.0.1", port, 10) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(sock.good());
        EXPECT_EQ(sock.getRemotePort(), port);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const char msg[] = "Hello Test !";
        EXPECT_FALSE(sock.write(msg, sizeof(msg)) <= 0);
        sock.disconnect();
    });

    tlistener.join();
    tsender.join();
}

TEST(TCP, NonBlocking) {
    std::thread tlistener([]() {
        qb::io::tcp::listener listener;
        EXPECT_FALSE(listener.listen(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.good());
        EXPECT_EQ(listener.getLocalPort(), port);
        qb::io::tcp::socket sock;
        EXPECT_FALSE(listener.accept(sock) != qb::io::SocketStatus::Done);
        sock.setBlocking(false);
        char buffer[512];
        *buffer = 0;

        EXPECT_FALSE(sock.read(buffer, 512) > 0);
        EXPECT_EQ(std::string(buffer), "");
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::thread tsender([]() {
        qb::io::tcp::socket sock;
        EXPECT_FALSE(sock.connect("127.0.0.1", port, 10) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(sock.good());
        EXPECT_EQ(sock.getRemotePort(), port);
        sock.setBlocking(false);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const char msg[] = "Hello Test !";
        EXPECT_FALSE(sock.write(msg, sizeof(msg)) <= 0);
        sock.disconnect();
    });

    tlistener.join();
    tsender.join();
}

TEST(UDP, Blocking) {
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.good());
        EXPECT_EQ(listener.getLocalPort(), port);
        char buffer[512];
        *buffer = 0;

        qb::io::ip from;
        unsigned short from_port;
        EXPECT_FALSE(listener.read(buffer, 512, from, from_port) <= 0);
        EXPECT_EQ(std::string(buffer), "Hello Test !");
        std::cout << "Received UDP from " << from << ":" << from_port << std::endl;
        listener.unbind();
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::thread tsender([]() {
        qb::io::udp::socket sock;
        EXPECT_TRUE(sock.good());
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const char msg[] = "Hello Test !";
        EXPECT_FALSE(sock.write(msg, sizeof(msg), "127.0.0.1", port) <= 0);
        sock.close();
    });

    tlistener.join();
    tsender.join();
}

TEST(UDP, NonBlocking) {
    std::thread tlistener([]() {
        qb::io::udp::socket listener;
        EXPECT_FALSE(listener.bind(port) != qb::io::SocketStatus::Done);
        EXPECT_TRUE(listener.good());
        EXPECT_EQ(listener.getLocalPort(), port);
        listener.setBlocking(false);
        char buffer[512];
        *buffer = 0;

        qb::io::ip from;
        unsigned short from_port;
        EXPECT_FALSE(listener.read(buffer, 512, from, from_port) > 0);
        EXPECT_EQ(std::string(buffer), "");
        listener.unbind();
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::thread tsender([]() {
        qb::io::udp::socket sock;
        EXPECT_TRUE(sock.good());
        sock.setBlocking(false);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const char msg[] = "Hello Test !";
        EXPECT_FALSE(sock.write(msg, sizeof(msg), "127.0.0.1", port) <= 0);
        sock.close();
    });

    tlistener.join();
    tsender.join();
}