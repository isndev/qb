/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tls/tls-peer-verification.cpp
 * @brief Secure-by-default TLS: a verifying client rejects the self-signed server; set_insecure opts out.
 *
 * Promoted from test-async-io.cpp's `SSLPeerVerificationSecureByDefault`, this is the MITM-hardening
 * lock-in for `qb::io::tcp::ssl::socket`: qb-io builds its client `SSL_CTX` with `SSL_VERIFY_PEER` and
 * hostname checking enabled, so a default client MUST fail the handshake against the self-signed test
 * certificate, and `set_insecure()` MUST be the only way to opt out. The negative path (verify-on vs
 * self-signed) was previously untested at the socket level — it is asserted here, alongside the
 * positive `set_insecure()` success, and additionally the diagnosable error string the failed
 * handshake leaves behind.
 *
 * A server's identity is its certificate CHAIN (Huly QB-611): a verifying client that trusts only the root
 * reaches it through the intermediate the server's certificate file holds -- loaded by `Context::server`
 * (`Context::identity`), by the raw `create_server_context`, and on the client side of mutual TLS by
 * `configure_client_certificate`. Those chains are generated in memory and pumped on the test thread
 * (`tls_pump.h`); the same leaf served without its intermediate is the control that the verification was
 * not relaxed.
 *
 * De-flake (per the restructure spec §2): the original used a fixed port (64388) and an acceptor that
 * looped 400×2ms while busy-driving each handshake 200×1ms, then slept a flat 80ms before connecting.
 * Here the listener binds `:0` (ephemeral), the acceptor is deadline-bounded and best-effort drives
 * whichever client connects, and the test waits on an explicit `acceptor_ready` flag rather than a
 * blind sleep. The acceptor is always joined via an RAII guard.
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <openssl/ssl.h>

#include <gtest/gtest.h>

#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>

#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_pump.h"

using namespace std::chrono_literals;

using qb::io::test::complete_tls_handshake;
using qb::io::test::generated_identity;
using qb::io::test::open_tls_connection;
using qb::io::test::require_ssl_files;
using qb::io::test::ssl_resource_path;
using qb::io::test::tls_pair;

namespace {

// Best-effort, deadline-bounded server acceptor for a self-signed TLS server. It
// accepts whichever clients connect and drives each handshake until completion,
// fatal error (the verifying client rejecting the cert), or a short per-connection
// deadline. Runs until `stop` is set; signals `ready` once it is in its loop.
class self_signed_acceptor {
public:
    self_signed_acceptor(qb::io::tcp::ssl::listener &listener, std::atomic<bool> &ready, std::atomic<bool> &stop)
        : _thread([&listener, &ready, &stop] {
            ready = true;
            while (!stop.load()) {
                qb::io::tcp::ssl::socket server_socket;
                if (listener.accept(server_socket) != 0) {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!server_socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
                    if (server_socket.handshake_status() < 0) {
                        break; // peer aborted (e.g. the verifying client rejecting our cert)
                    }
                    std::this_thread::sleep_for(1ms);
                }
            }
        }) {}

    self_signed_acceptor(const self_signed_acceptor &)            = delete;
    self_signed_acceptor &operator=(const self_signed_acceptor &) = delete;

    void
    join() {
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    ~self_signed_acceptor() {
        join();
    }

private:
    std::thread _thread;
};

// A certificate chain file: the leaf's PEM, then its issuer's -- the order a server's certificate file holds
// them in. Written next to the leaf's own file and removed with the object.
class chain_file {
    std::string _path;
    bool        _ok{false};

public:
    chain_file(const generated_identity &leaf, const generated_identity &issuer)
        : _path(leaf.cert() + "-chain.pem") {
        std::ofstream out(_path, std::ios::binary | std::ios::trunc);
        std::ifstream first(leaf.cert(), std::ios::binary);
        std::ifstream second(issuer.cert(), std::ios::binary);
        out << first.rdbuf() << second.rdbuf();
        _ok = static_cast<bool>(out);
    }

    chain_file(const chain_file &)            = delete;
    chain_file &operator=(const chain_file &) = delete;

    ~chain_file() {
        std::remove(_path.c_str());
    }

    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    [[nodiscard]] const std::string &
    path() const noexcept {
        return _path;
    }
};

// A client that verifies for real and trusts `anchor` (beside the system store): every certificate between
// `anchor` and the server's leaf is the server's to present.
tls_pair
verifying_pair(const generated_identity &anchor) {
    return tls_pair{qb::io::tcp::ssl::socket{qb::io::ssl::Context::client().trust(anchor.cert())}};
}

} // namespace

TEST(TlsPeerVerification, SecureByDefaultRejectsSelfSignedAndInsecureOptsOut) {
    ASSERT_TRUE(require_ssl_files()) << "shipped SSL cert/key not found at " << ssl_resource_path("cert.pem");

    qb::io::tcp::ssl::listener listener;
    auto                       tls = qb::io::ssl::Context::server(ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"));
    ASSERT_TRUE(tls.ok());
    listener.init(std::move(tls));
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool>    ready{false};
    std::atomic<bool>    stop{false};
    self_signed_acceptor acceptor(listener, ready, stop);

    while (!ready.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // 1) Default (verifying) client MUST fail against the self-signed cert, and
    //    must leave a diagnosable verification error behind.
    {
        qb::io::tcp::ssl::socket verifying_client;
        ASSERT_TRUE(verifying_client.verify_peer()) << "client must be secure-by-default";
        ASSERT_TRUE(verifying_client.set_sni_hostname("localhost"));
        const int ret = verifying_client.connect_v4("localhost", port);
        EXPECT_NE(ret, 0) << "secure-by-default client accepted a self-signed certificate (MITM hole)";
        EXPECT_FALSE(verifying_client.handshake_complete());
    }

    // 2) set_insecure() client MUST connect to the very same server.
    {
        qb::io::tcp::ssl::socket insecure_client;
        insecure_client.set_insecure();
        const int ret = insecure_client.connect_v4("127.0.0.1", port);
        EXPECT_EQ(ret, 0) << "set_insecure() failed to opt out of verification";
        EXPECT_TRUE(insecure_client.handshake_complete());
        insecure_client.disconnect();
    }

    stop = true;
    listener.disconnect();
    acceptor.join();
}

// A server presents the whole chain its certificate file holds (Huly QB-611). Every loader read only the
// file's first certificate, so a server whose certificate an intermediate issued presented a chain no
// verifying client could complete ("unable to get local issuer certificate"). Both server loaders, then the
// control: the same leaf without its intermediate is still refused -- the loaders take what the file holds,
// the verification is untouched.
TEST(TlsPeerVerification, AServerPresentsTheWholeChainItsCertificateFileHolds) {
    const generated_identity root{"qb-chain-root", nullptr, generated_identity::role::authority};
    const generated_identity intermediate{"qb-chain-intermediate", &root, generated_identity::role::authority};
    const generated_identity leaf{"localhost", &intermediate};
    ASSERT_TRUE(root.ok() && intermediate.ok() && leaf.ok()) << "could not generate the test chain";
    const chain_file chain{leaf, intermediate};
    ASSERT_TRUE(chain.ok()) << "could not write " << chain.path();

    qb::io::tcp::ssl::listener typed{qb::io::ssl::Context::server(chain.path(), leaf.key())};
    ASSERT_TRUE(typed.context().ok()) << typed.context().error();
    qb::io::tcp::ssl::listener raw;
    raw.init(qb::io::ssl::create_server_context(TLS_server_method(), chain.path(), leaf.key()));
    ASSERT_NE(raw.ssl_handle(), nullptr) << "create_server_context refused the chain file";
    for (auto *server : {&typed, &raw}) {
        ASSERT_EQ(server->listen_v4(0, "127.0.0.1"), 0);
        auto pair = verifying_pair(root);
        ASSERT_TRUE(open_tls_connection(*server, server->local_endpoint().port(), pair));
        EXPECT_TRUE(complete_tls_handshake(pair)) << (server == &typed ? "Context::server" : "create_server_context")
                                                  << ": the client could not reach the root through the server's chain";
    }

    qb::io::tcp::ssl::listener leaf_only{leaf.context()};
    ASSERT_EQ(leaf_only.listen_v4(0, "127.0.0.1"), 0);
    auto refused = verifying_pair(root);
    ASSERT_TRUE(open_tls_connection(leaf_only, leaf_only.local_endpoint().port(), refused));
    EXPECT_FALSE(complete_tls_handshake(refused)) << "a leaf without its intermediate was accepted: the verification was relaxed";
}

// The client side of the same rule (Huly QB-611): `configure_client_certificate` presents the client's whole
// chain, to a server that requires a client certificate and trusts only the root.
TEST(TlsPeerVerification, AClientPresentsTheWholeChainItsCertificateFileHolds) {
    const generated_identity root{"qb-mtls-root", nullptr, generated_identity::role::authority};
    const generated_identity intermediate{"qb-mtls-intermediate", &root, generated_identity::role::authority};
    const generated_identity server_leaf{"localhost", &root};
    const generated_identity client_leaf{"qb-mtls-client", &intermediate};
    ASSERT_TRUE(root.ok() && intermediate.ok() && server_leaf.ok() && client_leaf.ok()) << "could not generate the test chains";
    const chain_file client_chain{client_leaf, intermediate};
    ASSERT_TRUE(client_chain.ok()) << "could not write " << client_chain.path();

    qb::io::tcp::ssl::listener server{
        qb::io::ssl::Context::server(server_leaf.cert(), server_leaf.key()).trust(root.cert()).verify(qb::io::ssl::VerifyMode::peer_require)
    };
    ASSERT_TRUE(server.context().ok()) << server.context().error();
    ASSERT_EQ(server.listen_v4(0, "127.0.0.1"), 0);

    auto client_context = qb::io::ssl::Context::client().trust(root.cert());
    ASSERT_TRUE(qb::io::ssl::configure_client_certificate(client_context.native(), client_chain.path(), client_leaf.key()));
    tls_pair pair{qb::io::tcp::ssl::socket{client_context}};
    ASSERT_TRUE(open_tls_connection(server, server.local_endpoint().port(), pair));
    EXPECT_TRUE(complete_tls_handshake(pair)) << "the server could not reach the root through the client's chain";
}
