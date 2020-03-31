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
#include <qb/io/async.h>
#include <qb/io/protocol/cmd.h>
#include <qb/io/transport/file.h>
#include <thread>
#include <chrono>

using namespace qb::io;

class FileSession
        : public use<FileSession>::input<protocol::cmd<transport::file>>
        , public ostream<sys::file> {
public:

    void on(IOMessage msg, std::size_t size) {
        this->publish(msg, size);
    }
};

TEST(Session, DISABLED_FromStdinToStdout) {
    async::init();

    FileSession session;
    session.in().open(0);
    session.out().open(1);
    session.start();
    async::run(EVRUN_ONCE);
}

#ifndef _WIN32

TEST(Session, FromFileToStdout) {
    async::init();

    system("echo read from file > test.file");
    FileSession session;
    session.in().open("./test.file");
    session.out().open(1);
    session.start();

    async::run(EVRUN_ONCE);
}

#endif

class MyServer;

class MyClient : public use<MyClient>::tcp::client<protocol::cmd, MyServer> {
public:
    MyClient(MyServer &server)
            : client(server) {}

    void on(IOMessage msg, std::size_t size) {
        std::cout << "read " << size << " bytes" << std::endl;
    }
};

class MyServer : public use<MyServer>::tcp::server<MyClient> {
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

class TcpClient : public use<TcpClient>::tcp::client<protocol::cmd> {
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

#ifdef QB_IO_WITH_SSL
// secure session
class MySecureServer;

class MySecureClient : public use<MySecureClient>::tcp::ssl::client<protocol::cmd, MySecureServer> {
public:
    MySecureClient(MySecureServer &server)
            : client(server) {}

    void on(char const *message, std::size_t size) {
        std::cout << "read " << size << " bytes" << std::endl;
    }
};

void ShowCerts(SSL* ssl)
{
    X509 *cert;
    char *line;

    cert = SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
    if ( cert != NULL )
    {
        printf("Server certificates:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        printf("Subject: %s\n", line);
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        printf("Issuer: %s\n", line);
        free(line);
        X509_free(cert);
    }
    else
        printf("No certificates.\n");
}

class MySecureServer : public use<MySecureServer>::tcp::ssl::server<MySecureClient> {
public:

    void on(MySecureClient &client) {
        std::cout << "new client connected" << std::endl;

    }

};

TEST(Session, DISABLED_SecureTcpAccept) {
    async::init();

    MySecureServer server;
    server.in().init(tcp::ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        tcp::ssl::socket sock;
        sock.connect("127.0.0.1", 60123);
        const char msg[] = "hello world !\nfoo\nbar\n";
        sock.write(msg, sizeof(msg));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sock.close();
    });
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    t.join();
}

class SecureTcpClient : public use<SecureTcpClient>::tcp::ssl::client<protocol::cmd> {
public:

    void on(char const *message, std::size_t size) {
        std::cout << "client read " << size << " bytes" << std::endl;
    }

};

TEST(Session, DISABLED_SecureTcpConnect) {
    async::init();

    MySecureServer server;
    server.in().init(tcp::ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        async::init();
        SecureTcpClient client;
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
    async::run(EVRUN_ONCE);
    async::run(EVRUN_ONCE);
    t.join();
}

#endif