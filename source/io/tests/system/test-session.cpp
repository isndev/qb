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
#include <qb/io/async/listener.h>
#include <qb/io/async/listener.tpl>
#include <qb/io/async/server.h>
#include <qb/io/async/session.h>
#include <qb/io/prot/cmd.h>
#include <qb/io/prot/accept.h>
#include <thread>
#include <chrono>

using namespace qb::io;

class FileSession
        : public async::input<FileSession, prot::cmd<sys::file>>, public ostream<sys::file> {
    using base_t = async::input<FileSession, prot::cmd<sys::file>>;
public:
    FileSession(async::listener &handler) : base_t(handler) {}

    void on(char const *message, std::size_t size) {
        ::write(1, message, size);
        this->push(message, size);
    }

    bool disconnected() const { return true; }
};

TEST(Session, FromStdinToStdout) {
//    async::listener handler;
    FileSession session(async::listener::current);
    session.in().open(0);
    session.out().open(1);
    session.start();
    async::listener::current.loop(EVRUN_ONCE);
}

//TEST(Session, FromFileToStdout) {
//    system("echo mytest > test.file");
//    async::listener handler;
//    FileSession session(handler);
//    session.in().open("./test.file");
//    session.out().open(1);
//    session.start();
//    handler.loop(EVRUN_ONCE);
//}
//
//class MyServer;
//
//class MyClient : public async::session<MyClient, prot::cmd<tcp::socket>, MyServer> {
//    using base_t = async::session<MyClient, prot::cmd<tcp::socket>, MyServer>;
//public:
//    MyClient(async::listener &handler, MyServer &server)
//            : base_t(handler, server) {}
//
//    void on(char const *message, std::size_t size) {
//        const auto ret = write(1, message, size);
//        std::cout << "read " << ret << " bytes" << std::endl;
//    }
//};
//
//class MyServer : public async::server<MyServer, MyClient, prot::accept> {
//    using base_t = async::server<MyServer, MyClient, prot::accept>;
//public:
//
//    MyServer(async::listener &handler) : base_t(handler) {}
//
//    void on(MyClient &client) {
//        std::cout << "new client connected" << std::endl;
//    }
//};
//
//TEST(Session, TcpAccept) {
//    async::listener handler;
//    MyServer server(handler);
//    server.in().listen(60123);
//    server.start();
//
//    std::thread t([]() {
//        std::this_thread::sleep_for(std::chrono::seconds(1));
//        tcp::socket sock;
//        sock.connect("127.0.0.1", 60123);
//        const char msg[] = "hello world !\nfoo\nbar\n";
//        sock.write(msg, sizeof(msg));
//        std::this_thread::sleep_for(std::chrono::seconds(1));
//        sock.close();
//    });
//    handler.loop(EVRUN_ONCE);
//    handler.loop(EVRUN_ONCE);
//    t.join();
//}
//
//class TcpClient : public async::session<MyClient, prot::cmd<tcp::socket>> {
//    using base_t = async::session<MyClient, prot::cmd<tcp::socket>>;
//public:
//    TcpClient(async::listener &handler)
//            : base_t(handler) {}
//
//    void on(char const *message, std::size_t size) {
//        const auto ret = write(1, message, size);
//        std::cout << "client read " << ret << " bytes" << std::endl;
//    }
//};
//
//TEST(Session, TcpConnect) {
//    async::listener handler;
//    MyServer server(handler);
//    server.in().listen(60123);
//    server.start();
//
//    std::thread t([]() {
//        async::listener handler;
//        TcpClient client(handler);
//        if (SocketStatus::Done != client.in().connect("127.0.0.1", 60123)) {
//            throw std::runtime_error("could not connect");
//        }
//        client.start();
//
//        const char msg[] = "hello world !\nfoo\nbar\n";
//        client.push(msg, sizeof(msg));
//        handler.loop(EVRUN_ONCE);
//    });
//    handler.loop(EVRUN_ONCE);
//    handler.loop(EVRUN_ONCE);
//    t.join();
//}
