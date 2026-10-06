/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-context-reload.cpp
 * @brief `tcp::ssl::listener::reload_context`: a certificate renewed without a restart (Huly QB-205).
 *
 * The contract, one assertion per clause:
 *   - the connection accepted AFTER the reload presents the new certificate, through both `accept`
 *     overloads (the one `transport::saccept` calls, and the one returning a socket);
 *   - a connection accepted BEFORE it keeps the certificate it was minted with -- one already established,
 *     which goes on exchanging data, and one whose handshake had not started yet, which completes it
 *     with the previous certificate;
 *   - a replacement that failed to load (the renewal left half-done: a new certificate beside the old
 *     key, whose error names the mismatch; missing files; an empty context) is refused, and the listener
 *     goes on serving the certificate it had.
 *
 * Hermetic and single-threaded: two self-signed certificates are generated in memory (no shipped
 * resource names the certificate a connection must show), and every handshake is pumped from the test
 * thread -- client and server non-blocking, alternately -- so "accepted before the reload, handshaken
 * after" is an ORDER the test writes down, not a race it hopes to win. With no second thread there is
 * no teardown race either: every connection is closed by the thread that drove it, after its last
 * byte was read, which is why these tests need no `teardown_rendezvous`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

using namespace std::chrono_literals;

namespace {

bool
write_bio(BIO *bio, const std::string &path) {
    char      *data = nullptr;
    const long n    = BIO_get_mem_data(bio, &data);
    if (n <= 0)
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data, static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

// A self-signed EC P-256 certificate whose subject is `CN=<cn>`, written as PEM to two temporary files
// the object removes. PEM goes through a memory BIO and a std::ofstream: no FILE* crosses into OpenSSL
// (on Windows that needs applink.c in the executable).
class generated_identity {
    std::string _cert;
    std::string _key;
    bool        _ok{false};

public:
    explicit generated_identity(const std::string &cn) {
        const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base  = (std::filesystem::temp_directory_path() / ("qb-reload-" + cn + "-" + stamp)).string();
        _cert            = base + "-cert.pem";
        _key             = base + "-key.pem";

        EVP_PKEY *pkey = EVP_EC_gen("P-256");
        X509     *x509 = X509_new();
        bool      ok   = pkey && x509;
        if (ok) {
            ASN1_INTEGER_set(X509_get_serialNumber(x509), static_cast<long>(cn.size()) + 1);
            X509_gmtime_adj(X509_getm_notBefore(x509), -3600);
            X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
            X509_NAME *name = X509_get_subject_name(x509);
            ok = X509_set_pubkey(x509, pkey) == 1
                 && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0) == 1
                 && X509_set_issuer_name(x509, name) == 1 && X509_sign(x509, pkey, EVP_sha256()) != 0;
        }
        if (ok) {
            BIO *cb = BIO_new(BIO_s_mem());
            BIO *kb = BIO_new(BIO_s_mem());
            ok = cb && kb && PEM_write_bio_X509(cb, x509) == 1 && PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1
                 && write_bio(cb, _cert) && write_bio(kb, _key);
            BIO_free(cb);
            BIO_free(kb);
        }
        X509_free(x509);
        EVP_PKEY_free(pkey);
        _ok = ok;
    }

    generated_identity(const generated_identity &)            = delete;
    generated_identity &operator=(const generated_identity &) = delete;

    ~generated_identity() {
        std::remove(_cert.c_str());
        std::remove(_key.c_str());
    }

    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    [[nodiscard]] const std::string &
    cert() const noexcept {
        return _cert;
    }

    [[nodiscard]] const std::string &
    key() const noexcept {
        return _key;
    }

    [[nodiscard]] qb::io::ssl::Context
    context() const {
        return qb::io::ssl::Context::server(_cert, _key);
    }
};

// A TCP connection to the listener, accepted but with no TLS byte exchanged yet: the client is
// non-blocking with its SSL state set up, the server side holds the SSL the listener minted.
struct pending_connection {
    qb::io::tcp::ssl::socket client;
    qb::io::tcp::ssl::socket server;
};

enum class accept_form { into_socket, returning_socket };

::testing::AssertionResult
open_connection(qb::io::tcp::ssl::listener &listener, uint16_t port, accept_form form, pending_connection &out) {
    out.client.set_insecure(); // the certificates are self-signed: what is under test is WHICH one is shown
    const int ret = out.client.n_connect_v4("127.0.0.1", port);
    const int err = qb::io::socket::get_last_errno();
    if (ret != 0 && !qb::io::socket_no_error(err))
        return ::testing::AssertionFailure() << "n_connect_v4 failed: ret=" << ret << " errno=" << err;
    if (form == accept_form::into_socket) {
        if (listener.accept(out.server) != 0)
            return ::testing::AssertionFailure() << "accept(ssl::socket&) failed";
    } else {
        out.server = listener.accept();
        if (!out.server.is_open())
            return ::testing::AssertionFailure() << "accept() returned a closed socket";
    }
    out.server.set_nonblocking(true);
    return ::testing::AssertionSuccess();
}

// Pump both ends, alternately, until both report the handshake complete.
::testing::AssertionResult
complete_handshake(pending_connection &c, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!(c.client.handshake_complete() && c.server.handshake_complete())) {
        if (std::chrono::steady_clock::now() >= deadline)
            return ::testing::AssertionFailure() << "the handshake did not complete within the deadline";
        if (!c.client.handshake_complete() && c.client.handshake_status() < 0)
            return ::testing::AssertionFailure() << "client handshake failed: " << c.client.get_last_ssl_error_string();
        if (!c.server.handshake_complete() && c.server.handshake_status() < 0)
            return ::testing::AssertionFailure() << "server handshake failed: " << c.server.get_last_ssl_error_string();
        std::this_thread::sleep_for(1ms);
    }
    return ::testing::AssertionSuccess();
}

std::string
presented_name(const qb::io::tcp::ssl::socket &client) {
    return client.get_peer_certificate_details().subject; // X509_NAME_oneline: "/CN=<cn>"
}

// One message each way over an established connection, both ends non-blocking.
::testing::AssertionResult
round_trip(pending_connection &c, std::string_view ping, std::string_view pong, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto transfer = [&](qb::io::tcp::ssl::socket &from, qb::io::tcp::ssl::socket &to, std::string_view what) -> ::testing::AssertionResult {
        std::size_t sent = 0;
        std::string got;
        char        buffer[64];
        while (got.size() < what.size()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return ::testing::AssertionFailure() << "timed out moving \"" << what << "\": got \"" << got << "\"";
            if (sent < what.size()) {
                const int w = from.write(what.data() + sent, what.size() - sent);
                if (w < 0)
                    return ::testing::AssertionFailure() << "write failed: " << from.get_last_ssl_error_string();
                sent += static_cast<std::size_t>(w);
            }
            const int r = to.read(buffer, sizeof(buffer));
            if (r < 0)
                return ::testing::AssertionFailure() << "read failed: " << to.get_last_ssl_error_string();
            got.append(buffer, static_cast<std::size_t>(r));
            if (r == 0)
                std::this_thread::sleep_for(1ms);
        }
        if (got != what)
            return ::testing::AssertionFailure() << "expected \"" << what << "\", got \"" << got << "\"";
        return ::testing::AssertionSuccess();
    };
    auto there = transfer(c.client, c.server, ping);
    if (!there)
        return there;
    return transfer(c.server, c.client, pong);
}

} // namespace

TEST(TlsContextReload, TheNextConnectionGetsTheNewCertificateAndTheOpenOnesKeepTheirs) {
    const generated_identity before{"qb-reload-before"};
    const generated_identity after{"qb-reload-after"};
    ASSERT_TRUE(before.ok() && after.ok()) << "could not generate the two test certificates";

    qb::io::tcp::ssl::listener listener{before.context()};
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    // Established before the reload.
    pending_connection established;
    ASSERT_TRUE(open_connection(listener, port, accept_form::into_socket, established));
    ASSERT_TRUE(complete_handshake(established));
    EXPECT_NE(presented_name(established.client).find("CN=qb-reload-before"), std::string::npos) << presented_name(established.client);

    // Accepted before the reload -- the listener has minted its SSL -- but no TLS byte exchanged yet.
    pending_connection accepted_only;
    ASSERT_TRUE(open_connection(listener, port, accept_form::into_socket, accepted_only));

    SSL_CTX *const previous = listener.ssl_handle();
    const auto     renewed  = after.context();
    ASSERT_TRUE(renewed.ok()) << renewed.error();
    ASSERT_TRUE(listener.reload_context(renewed));
    EXPECT_EQ(listener.ssl_handle(), renewed.native());
    EXPECT_NE(listener.ssl_handle(), previous);

    // The one accepted before completes its handshake with the certificate it was minted with.
    ASSERT_TRUE(complete_handshake(accepted_only));
    EXPECT_NE(presented_name(accepted_only.client).find("CN=qb-reload-before"), std::string::npos) << presented_name(accepted_only.client);

    // A connection accepted after it presents the new certificate, through each accept overload.
    for (const auto form : {accept_form::into_socket, accept_form::returning_socket}) {
        pending_connection fresh;
        ASSERT_TRUE(open_connection(listener, port, form, fresh));
        ASSERT_TRUE(complete_handshake(fresh));
        EXPECT_NE(presented_name(fresh.client).find("CN=qb-reload-after"), std::string::npos)
            << "accept form " << static_cast<int>(form) << " presented " << presented_name(fresh.client);
        EXPECT_TRUE(round_trip(fresh, "new?", "new!"));
    }

    // The connections opened before the reload are untouched by it.
    EXPECT_TRUE(round_trip(established, "ping", "pong"));
    EXPECT_TRUE(round_trip(accepted_only, "late", "fine"));
}

TEST(TlsContextReload, AContextThatFailedToLoadIsRefusedAndTheCurrentOneServes) {
    const generated_identity current{"qb-reload-current"};
    ASSERT_TRUE(current.ok()) << "could not generate the test certificate";

    qb::io::tcp::ssl::listener listener{current.context()};
    ASSERT_TRUE(listener.context().ok()) << listener.context().error();
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);
    SSL_CTX *const serving = listener.ssl_handle();

    // A renewal left half-done -- the new certificate beside the old key -- whose error names the mismatch
    // (OpenSSL's reason, not only the file); a renewal whose files are not there; no context at all.
    const generated_identity renewed{"qb-reload-renewed"};
    ASSERT_TRUE(renewed.ok()) << "could not generate the renewed certificate";
    const auto half = qb::io::ssl::Context::server(renewed.cert(), current.key());
    ASSERT_FALSE(half.ok());
    EXPECT_NE(half.error().find("mismatch"), std::string::npos) << half.error();
    EXPECT_FALSE(listener.reload_context(half));
    const auto missing = qb::io::ssl::Context::server("qb-reload-no-such-cert.pem", "qb-reload-no-such-key.pem");
    ASSERT_FALSE(missing.ok());
    EXPECT_FALSE(listener.reload_context(missing));
    EXPECT_FALSE(listener.reload_context(qb::io::ssl::Context{}));
    EXPECT_EQ(listener.ssl_handle(), serving);

    pending_connection next;
    ASSERT_TRUE(open_connection(listener, port, accept_form::into_socket, next));
    ASSERT_TRUE(complete_handshake(next));
    EXPECT_NE(presented_name(next.client).find("CN=qb-reload-current"), std::string::npos) << presented_name(next.client);
    EXPECT_TRUE(round_trip(next, "ping", "pong"));
}
