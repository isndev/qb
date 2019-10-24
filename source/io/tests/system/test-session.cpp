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
#include <qb/io/async/tcp/server.h>
#include <qb/io/async/tcp/session.h>
#include <qb/io/protocol/cmd.h>
#include <qb/io/protocol/file.h>
#include <qb/io/protocol/accept.h>
#include <thread>
#include <chrono>

using namespace qb::io;

class FileSession
        : public async::input<FileSession, protocol::cmd<protocol::file>>
        , public ostream<sys::file> {
public:

    void on(char const *message, std::size_t size) {
        this->publish(message, size);
    }

    bool disconnected() const { return true; }
};

TEST(Session, FromStdinToStdout) {
    async::init();

    FileSession session;
    session.in().open(0);
    session.out().open(1);
    session.start();
    async::run(EVRUN_ONCE);
}

TEST(Session, FromFileToStdout) {
    async::init();

    system("echo read from file > test.file");
    FileSession session;
    session.in().open("./test.file");
    session.out().open(1);
    session.start();

    async::run(EVRUN_ONCE);
}

class MyServer;

class MyClient : public async::tcp::session<MyClient, protocol::cmd, MyServer> {
    using base_t = async::tcp::session<MyClient, protocol::cmd, MyServer>;
public:
    MyClient(MyServer &server)
            : base_t(server) {}

    void on(char const *message, std::size_t size) {
        std::cout << "read " << size << " bytes" << std::endl;
    }
};

class MyServer : public async::tcp::server<MyServer, MyClient> {
    using base_t = async::tcp::server<MyServer, MyClient>;
public:

    void on(MyClient &client) {
        std::cout << "new client connected" << std::endl;
    }
};

TEST(Session, TcpAccept) {
    async::init();

    MyServer server;
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        tcp::socket sock;
        sock.connect("127.0.0.1", 60123);
        const char msg[] = "hello world !\nfoo\nbar\n";
        sock.write(msg, sizeof(msg));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sock.close();
    });
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    t.join();
}

class TcpClient : public async::tcp::session<MyClient, protocol::cmd> {
    using base_t = async::tcp::session<MyClient, protocol::cmd>;
public:

    void on(char const *message, std::size_t size) {
        std::cout << "client read " << size << " bytes" << std::endl;
    }
};

TEST(Session, TcpConnect) {
    async::init();

    MyServer server;
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        async::init();
        TcpClient client;
        if (SocketStatus::Done != client.in().connect("127.0.0.1", 60123)) {
            throw std::runtime_error("could not connect");
        }
        client.start();

        const char msg[] = "hello world !\nfoo\nbar\n";
        client.publish(msg, sizeof(msg));
        async::run(EVRUN_ONCE);
    });
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    t.join();
}
