/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/tls_pump.h
 * @brief A single-threaded TLS loopback harness: generated certificates, both ends pumped in turn.
 *
 * The two-thread harnesses of the older TLS tests (one thread per end, each driving its own handshake)
 * race at teardown and were kept off Windows for it. Here the test thread holds both ends, non-blocking,
 * and pumps them alternately: the order of every step -- accepted, configured, handshaken, closed -- is
 * the order the test writes, on every platform, and a connection is closed by the thread that drove it
 * after its last byte was read, so no `teardown_rendezvous` is needed. Certificates are generated in
 * memory, so a test can tell WHICH certificate a connection was shown without a resource naming it.
 * Used by system/tls/tls-context-reload.cpp (Huly QB-205) and system/tls/tls-ocsp-callbacks.cpp (Huly QB-83).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Tests
 */

#ifndef QB_IO_TESTS_SHARED_TLS_PUMP_H
#define QB_IO_TESTS_SHARED_TLS_PUMP_H

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

namespace qb::io::test {

namespace tls_pump_detail {

inline bool
write_bio(BIO *bio, const std::string &path) {
    char      *data = nullptr;
    const long n    = BIO_get_mem_data(bio, &data);
    if (n <= 0)
        return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data, static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

} // namespace tls_pump_detail

// An EC P-256 certificate whose subject is `CN=<cn>`, self-signed or issued by another identity, written as
// PEM to two temporary files the object removes; `authority` makes it a CA (`basicConstraints CA:TRUE`). The
// certificate and key stay readable (`x509()`, `pkey()`) for what a test signs with them -- an OCSP response.
// PEM goes through a memory BIO and a std::ofstream: no FILE* crosses into OpenSSL (on Windows that needs
// applink.c in the executable).
class generated_identity {
    std::string _cert;
    std::string _key;
    EVP_PKEY   *_pkey{nullptr};
    X509       *_x509{nullptr};
    bool        _ok{false};

public:
    enum class role { leaf, authority };

    explicit generated_identity(const std::string &cn, const generated_identity *issuer = nullptr, role r = role::leaf) {
        static long serial = 0;
        const auto  stamp  = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto  base   = (std::filesystem::temp_directory_path() / ("qb-tls-" + cn + "-" + stamp)).string();
        _cert              = base + "-cert.pem";
        _key               = base + "-key.pem";

        _pkey   = EVP_EC_gen("P-256");
        _x509   = X509_new();
        bool ok = _pkey && _x509 && (!issuer || issuer->ok());
        if (ok) {
            ASN1_INTEGER_set(X509_get_serialNumber(_x509), ++serial);
            X509_set_version(_x509, 2); // v3: extensions
            X509_gmtime_adj(X509_getm_notBefore(_x509), -3600);
            X509_gmtime_adj(X509_getm_notAfter(_x509), 3600);
            X509_NAME *name = X509_get_subject_name(_x509);
            ok = X509_set_pubkey(_x509, _pkey) == 1
                 && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(cn.c_str()), -1, -1, 0) == 1
                 && X509_set_issuer_name(_x509, issuer ? X509_get_subject_name(issuer->_x509) : name) == 1;
        }
        if (ok && r == role::authority) {
            BASIC_CONSTRAINTS *bc = BASIC_CONSTRAINTS_new();
            ok                    = bc && (bc->ca = 1, X509_add1_ext_i2d(_x509, NID_basic_constraints, bc, 1, 0) == 1);
            BASIC_CONSTRAINTS_free(bc);
        }
        ok = ok && X509_sign(_x509, issuer ? issuer->_pkey : _pkey, EVP_sha256()) != 0;
        if (ok) {
            BIO *cb = BIO_new(BIO_s_mem());
            BIO *kb = BIO_new(BIO_s_mem());
            ok = cb && kb && PEM_write_bio_X509(cb, _x509) == 1
                 && PEM_write_bio_PrivateKey(kb, _pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1 && tls_pump_detail::write_bio(cb, _cert)
                 && tls_pump_detail::write_bio(kb, _key);
            BIO_free(cb);
            BIO_free(kb);
        }
        _ok = ok;
    }

    generated_identity(const generated_identity &)            = delete;
    generated_identity &operator=(const generated_identity &) = delete;

    ~generated_identity() {
        X509_free(_x509);
        EVP_PKEY_free(_pkey);
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

    [[nodiscard]] X509 *
    x509() const noexcept {
        return _x509;
    }

    [[nodiscard]] EVP_PKEY *
    pkey() const noexcept {
        return _pkey;
    }

    [[nodiscard]] qb::io::ssl::Context
    context() const {
        return qb::io::ssl::Context::server(_cert, _key);
    }
};

// A TCP connection to a listener, accepted but with no TLS byte exchanged yet: the client non-blocking
// with its SSL state set up, the server side holding the SSL the listener minted. The client socket is
// the caller's to build before `open_tls_connection` (a `Context`, `set_insecure()`, ...).
struct tls_pair {
    qb::io::tcp::ssl::socket client;
    qb::io::tcp::ssl::socket server;

    tls_pair() = default;
    explicit tls_pair(qb::io::tcp::ssl::socket c)
        : client(std::move(c)) {}
};

enum class accept_form { into_socket, returning_socket };

// Connect `pair.client` to 127.0.0.1:`port` with `servername` as its SNI and verification name, and accept
// the connection on `listener` through the given overload.
inline ::testing::AssertionResult
open_tls_connection(qb::io::tcp::ssl::listener &listener, uint16_t port, tls_pair &pair, accept_form form = accept_form::into_socket,
                    const std::string &servername = "localhost") {
    const int ret = pair.client.n_connect(qb::io::endpoint{"127.0.0.1", port}, servername);
    const int err = qb::io::socket::get_last_errno();
    if (ret != 0 && !qb::io::socket_no_error(err))
        return ::testing::AssertionFailure() << "n_connect failed: ret=" << ret << " errno=" << err;
    if (form == accept_form::into_socket) {
        if (listener.accept(pair.server) != 0)
            return ::testing::AssertionFailure() << "accept(ssl::socket&) failed";
    } else {
        pair.server = listener.accept();
        if (!pair.server.is_open())
            return ::testing::AssertionFailure() << "accept() returned a closed socket";
    }
    pair.server.set_nonblocking(true);
    return ::testing::AssertionSuccess();
}

// Pump both ends, alternately, until both report the handshake complete -- or one of them fails it.
inline ::testing::AssertionResult
complete_tls_handshake(tls_pair &pair, std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!(pair.client.handshake_complete() && pair.server.handshake_complete())) {
        if (std::chrono::steady_clock::now() >= deadline)
            return ::testing::AssertionFailure() << "the handshake did not complete within the deadline";
        if (!pair.client.handshake_complete() && pair.client.handshake_status() < 0)
            return ::testing::AssertionFailure() << "client handshake failed: " << pair.client.get_last_ssl_error_string();
        if (!pair.server.handshake_complete() && pair.server.handshake_status() < 0)
            return ::testing::AssertionFailure() << "server handshake failed: " << pair.server.get_last_ssl_error_string();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return ::testing::AssertionSuccess();
}

// One message each way over an established pair, both ends non-blocking.
inline ::testing::AssertionResult
tls_round_trip(tls_pair &pair, std::string_view ping, std::string_view pong, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
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
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (got != what)
            return ::testing::AssertionFailure() << "expected \"" << what << "\", got \"" << got << "\"";
        return ::testing::AssertionSuccess();
    };
    auto there = transfer(pair.client, pair.server, ping);
    if (!there)
        return there;
    return transfer(pair.server, pair.client, pong);
}

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_TLS_PUMP_H
