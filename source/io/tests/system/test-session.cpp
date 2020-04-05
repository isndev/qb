/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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
    std::size_t message_count = 0u;
public:
    explicit MyClient(MyServer &server)
            : client(server) {}

    ~MyClient() {
        EXPECT_EQ(message_count, 4u);
    }

    void on(IOMessage , std::size_t size) {
        EXPECT_EQ(size, 4u);
        ++message_count;
    }
};

class MyServer : public use<MyServer>::tcp::server<MyClient> {
    std::size_t connection_count = 0u;
public:

    ~MyServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void on(MyClient &) {
        ++connection_count;
    }
};

TEST(Session, TcpAccept) {
    async::init();

    MyServer server;
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        tcp::socket sock;
        sock.connect("127.0.0.1", 60123);
        const char msg[] = "isn\ndev\nfoo\nbar\n";
        sock.write(msg, sizeof(msg));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        sock.disconnect();
    });
    for (auto i = 0; i < 5; ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

class TcpClient : public use<TcpClient>::tcp::client<protocol::cmd> {
public:
    void on(char const *, std::size_t size) {
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

        const char msg[] = "isn\ndev\nfoo\nbar\n";
        client.publish(msg, sizeof(msg));
        async::run(EVRUN_ONCE);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        client.in().disconnect();
    });
    for (auto i = 0; i < 5; ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

#ifdef QB_IO_WITH_SSL
// secure session
class MySecureServer;

class MySecureClient : public use<MySecureClient>::tcp::ssl::client<protocol::cmd, MySecureServer> {
    std::size_t message_count = 0u;
public:
    explicit MySecureClient(MySecureServer &server)
            : client(server) {}

    ~MySecureClient() {
        EXPECT_EQ(message_count, 4u);
    }

    void on(char const *, std::size_t size) {
        EXPECT_EQ(size, 4u);
        ++message_count;
    }
};

void ShowCerts(SSL* ssl)
{
    X509 *cert;
    char *line;

    cert = SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
    if ( cert != nullptr )
    {
        printf("Server certificates:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        printf("Subject: %s\n", line);
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
        printf("Issuer: %s\n", line);
        free(line);
        printf("Version: %ld\n", X509_get_version(cert));
        X509_free(cert);
    }
    else
        printf("No certificates.\n");
}

class MySecureServer : public use<MySecureServer>::tcp::ssl::server<MySecureClient> {
    std::size_t connection_count = 0u;
public:

    ~MySecureServer() {
        EXPECT_EQ(connection_count, 1u);
    }

    void on(MySecureClient &) {
        ++connection_count;
    }

};

TEST(Session, SecureTcpAccept) {
    async::init();

    MySecureServer server;
    server.in().init(tcp::ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        tcp::ssl::socket sock;
        sock.connect("127.0.0.1", 60123);
        const char msg[] = "isn\ndev\nfoo\nbar\n";
        while (!sock.write(msg, sizeof(msg)));
        std::this_thread::sleep_for(std::chrono::seconds(3));
        sock.disconnect();
    });
    for (auto i = 0; i < 5; ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

class SecureTcpClient : public use<SecureTcpClient>::tcp::ssl::client<protocol::cmd> {
public:
    void on(char const *, std::size_t size) {
    }
};

TEST(Session, SecureTcpConnect) {
    async::init();

    MySecureServer server;
    server.in().init(tcp::ssl::create_server_context(SSLv23_server_method(), "cert.pem", "key.pem"));
    server.in().listen(60123);
    server.start();

    std::thread t([]() {
        async::init();
        // try another method for client
        auto ctx = tcp::ssl::create_client_context(TLS_client_method());
        {
            SecureTcpClient client;
            // optional init with custom SSL * handler
            client.in().init(SSL_new(ctx));
            if (SocketStatus::Done != client.in().connect("127.0.0.1", 60123)) {
                throw std::runtime_error("could not connect");
            }
            ShowCerts(client.in().ssl());
            client.start();

            const char msg[] = "isn\ndev\nfoo\nbar\n";
            client.publish(msg, sizeof(msg));
            async::run(EVRUN_ONCE);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            client.in().disconnect();
        }
        SSL_CTX_free(ctx);
    });
    for (auto i = 0; i < 5; ++i)
        async::run(EVRUN_ONCE);
    t.join();
}

#endif